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

// llamafile ZIM REGISTRY + BROWSER + TOOLS — the in-process home for one or more
// offline ZIM archives (`--zim PATH`, repeatable). It owns, for each ZIM:
//   * the zim_archive handle (article HTML/Markdown/text),
//   * its embedded Xapian BM25 indexes (X/fulltext/xapian + X/title/xapian),
// and exposes three things off that single shared set of handles:
//
//   1. HTTP routes  GET /zim/<id>/<path>  (and the back-compat /wiki/<path>
//      alias to the first ZIM) — stream an article's ORIGINAL HTML straight
//      from the archive, rewriting internal links to /zim/<id>/... so you can
//      click through the offline encyclopedia inside the binary.
//   2. In-process /tools  (zim_search, zim_get_article, zim_open, list_zims)
//      registered into the main server's tool registry — so the agentic loop,
//      the model and external clients all share ONE set of open ZIMs.
//   3. The Xapian-backed full-text ranking that makes long natural-language
//      queries ("TNT inventor") resolve to the right article.
//
// Everything runs in the MAIN --server process. The zim reader's cluster cache
// is not thread-safe, so a single mutex (g_zim_mu) guards every reader touch
// (routes AND tools, which run on HTTP worker threads / the agent loop).

#include "wiki_route.h"

#include "zim/zim.h"
#include "zim/zim_xapian.h"

#include "server-http.h"
#include "server-tools.h"  // server_tool, server_tools

#include <snowball/include/libstemmer.h>  // vendored Snowball stemmers (UTF-8)

#include <nlohmann/json.hpp>

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <strings.h>
#include <vector>

using json = nlohmann::ordered_json;

namespace {

// ---------------------------------------------------------------------------
// Multi-ZIM registry
// ---------------------------------------------------------------------------

struct ZimHandle {
    std::string  id;                 // filename stem, e.g. "wikipedia"
    std::string  path;               // path passed to --zim
    zim_archive *z = nullptr;        // article reader
    zim_xapian * fulltext = nullptr; // X/fulltext/xapian (BM25 over bodies)
    zim_xapian * title = nullptr;    // X/title/xapian (BM25 over titles)
    sb_stemmer * stemmer = nullptr;  // Snowball stemmer for this ZIM's language
    bool open_attempted = false;
    bool xapian_attempted = false;
};

std::vector<std::string> g_zim_paths;   // requested via --zim (repeatable)
std::vector<ZimHandle>   g_zims;        // opened archives
bool       g_wiki_enabled = false;
bool       g_zims_opened = false;
std::mutex g_zim_mu;  // the zim reader's cluster cache is not thread-safe

// filename stem (basename, drop directory + a trailing ".zim").
std::string path_stem(const std::string & p) {
    size_t slash = p.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
    if (base.size() > 4 && !strcasecmp(base.c_str() + base.size() - 4, ".zim")) {
        base.resize(base.size() - 4);
    }
    if (base.empty()) base = "zim";
    return base;
}

// Open every requested ZIM once (idempotent). Caller holds g_zim_mu.
void ensure_zims_open() {
    if (g_zims_opened) return;
    g_zims_opened = true;
    for (const auto & path : g_zim_paths) {
        ZimHandle h;
        h.path = path;
        h.id   = path_stem(path);
        // de-dup ids so /zim/<id>/ stays unambiguous
        std::string base = h.id;
        int n = 2;
        for (;;) {
            bool clash = false;
            for (const auto & e : g_zims) if (e.id == h.id) { clash = true; break; }
            if (!clash) break;
            h.id = base + "_" + std::to_string(n++);
        }
        h.z = zim_open(path.c_str());
        if (!h.z) {
            fprintf(stderr, "zim: failed to open '%s' in-process: %s\n",
                    path.c_str(), zim_error());
            continue;
        }
        g_zims.push_back(std::move(h));
    }
}

// forward decl (defined below, near list_zims)
std::string zim_metadata(zim_archive * z, const char * key);

// Map a ZIM `M/Language` value to a vendored Snowball stemmer algorithm name,
// or nullptr if there is no stemmer for that language (incl. CJK ja/zh/ko, which
// are not stemmed — they just search unstemmed). ZIM language is normally an
// ISO 639-3 code (e.g. "eng","fra","ara"); some archives use 639-1 ("en","fr").
// A multi-language value ("eng,fra") is keyed on its first code. Only the
// languages whose stemmers we actually vendor (UTF-8 set) are mapped.
const char * snowball_algo_for_language(const std::string & raw) {
    // take the first code, lowercase, keep ASCII letters only
    std::string c;
    for (char ch : raw) {
        if (isalpha((unsigned char) ch)) c.push_back((char) tolower((unsigned char) ch));
        else if (!c.empty()) break;  // stop at the first separator after a code
    }
    struct { const char * code; const char * algo; } map[] = {
        // ISO 639-3                      // ISO 639-1
        {"eng", "english"},   {"en", "english"},
        {"fra", "french"},    {"fre", "french"},     {"fr", "french"},
        {"spa", "spanish"},   {"es", "spanish"},
        {"ara", "arabic"},    {"ar", "arabic"},
        {"deu", "german"},    {"ger", "german"},     {"de", "german"},
        {"ita", "italian"},   {"it", "italian"},
        {"por", "portuguese"},{"pt", "portuguese"},
        {"rus", "russian"},   {"ru", "russian"},
        {"nld", "dutch"},     {"dut", "dutch"},      {"nl", "dutch"},
        {"swe", "swedish"},   {"sv", "swedish"},
        {"fin", "finnish"},   {"fi", "finnish"},
        {"tur", "turkish"},   {"tr", "turkish"},
        {"dan", "danish"},    {"da", "danish"},
        {"nor", "norwegian"}, {"nob", "norwegian"},  {"nno", "norwegian"}, {"no", "norwegian"},
        {"ron", "romanian"},  {"rum", "romanian"},   {"ro", "romanian"},
        {"hun", "hungarian"}, {"hu", "hungarian"},
        {"ell", "greek"},     {"gre", "greek"},      {"el", "greek"},
        {"cat", "catalan"},   {"ca", "catalan"},
        {"eus", "basque"},    {"baq", "basque"},     {"eu", "basque"},
        {"hin", "hindi"},     {"hi", "hindi"},
        {"ind", "indonesian"},{"id", "indonesian"},
        {"gle", "irish"},     {"ga", "irish"},
        {"lit", "lithuanian"},{"lt", "lithuanian"},
        {"nep", "nepali"},    {"ne", "nepali"},
        {"tam", "tamil"},     {"ta", "tamil"},
    };
    for (auto & m : map)
        if (c == m.code) return m.algo;
    return nullptr;  // unknown / CJK / unstemmed
}

// zim_xapian_stem_fn adapter: stem one lowercased UTF-8 term with the per-ZIM
// Snowball stemmer (passed as ctx). Writes the BARE stem into buf. The stemmer
// is stateful and NOT thread-safe; this runs under g_zim_mu (every search does).
const char * zim_stem_cb(void * ctx, const char * term, char * buf, size_t buflen) {
    sb_stemmer * st = (sb_stemmer *) ctx;
    if (!st || !term) return nullptr;
    int len = (int) strlen(term);
    const sb_symbol * out = sb_stemmer_stem(st, (const sb_symbol *) term, len);
    if (!out) return nullptr;
    int olen = sb_stemmer_length(st);
    if (olen <= 0 || (size_t) olen + 1 > buflen) return nullptr;
    memcpy(buf, out, (size_t) olen);
    buf[olen] = '\0';
    return buf;
}

// Lazily open the Xapian indexes for one ZIM (tolerant: a ZIM without an index
// just keeps null pointers and falls back to the title listing). Caller holds
// g_zim_mu.
void ensure_xapian(ZimHandle & h) {
    if (h.xapian_attempted) return;
    h.xapian_attempted = true;
    h.fulltext = zim_xapian_open(h.z, "X/fulltext/xapian");
    h.title    = zim_xapian_open(h.z, "X/title/xapian");
    if (!h.fulltext) {
        fprintf(stderr, "zim[%s]: no X/fulltext/xapian index (%s); "
                        "falling back to title search\n",
                h.id.c_str(), zim_xapian_error());
    }
    // Attach a Snowball stemmer keyed on the ZIM's declared language so a query
    // term also matches its morphological variants (STEM_SOME). Languages with
    // no vendored stemmer (incl. CJK) stay unstemmed.
    if (h.fulltext) {
        std::string lang = zim_metadata(h.z, "Language");
        const char * algo = snowball_algo_for_language(lang);
        if (algo) {
            h.stemmer = sb_stemmer_new(algo, "UTF_8");
            if (h.stemmer) {
                zim_xapian_set_stemmer(h.fulltext, zim_stem_cb, h.stemmer);
                fprintf(stderr, "zim[%s]: stemming queries with the %s "
                                "Snowball stemmer (language '%s')\n",
                        h.id.c_str(), algo, lang.c_str());
            } else {
                fprintf(stderr, "zim[%s]: Snowball '%s' stemmer unavailable; "
                                "searching unstemmed\n", h.id.c_str(), algo);
            }
        }
    }
}

ZimHandle * find_zim(const std::string & id) {
    for (auto & h : g_zims) if (h.id == id) return &h;
    return nullptr;
}

// ---------------------------------------------------------------------------
// small text helpers
// ---------------------------------------------------------------------------

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

std::string collapse_ws(const char * s, size_t n) {
    std::string out;
    out.reserve(n);
    bool in_ws = true;
    for (size_t i = 0; i < n && s[i]; i++) {
        unsigned char c = (unsigned char) s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            if (!in_ws) { out.push_back(' '); in_ws = true; }
        } else {
            out.push_back((char) c);
            in_ws = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Percent-encode the few characters that would break a URL path component
// (keep '/' and the safe ASCII set; the route URL-decodes on the way back in).
std::string url_encode_path(const char * path) {
    std::string out;
    for (const unsigned char * p = (const unsigned char *) path; *p; p++) {
        unsigned char c = *p;
        if (c == ' ') out += "%20";
        else if (c == '#') out += "%23";
        else if (c == '?') out += "%3F";
        else if (c == '%') out += "%25";
        else out.push_back((char) c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// article resolution (shared by the tools)
// ---------------------------------------------------------------------------
//
// Resolve a title-or-path to a content entry inside ONE zim, following
// redirects. Tries the bare path first (modern "nons" ZIMs store flat paths
// like "Paris"), then the namespace-qualified path ('C' new / 'A' old), then a
// title search. Returns true and fills *e (redirect-resolved) on success.
bool resolve_in_zim(ZimHandle & h, const std::string & title, zim_entry * e) {
    bool found = false;
    if (zim_get_entry_by_path(h.z, title.c_str(), e)) {
        found = true;
    } else {
        std::string c = "C/" + title, a = "A/" + title;
        if (zim_get_entry_by_path(h.z, c.c_str(), e) ||
            zim_get_entry_by_path(h.z, a.c_str(), e)) {
            found = true;
        }
    }
    if (!found) {
        zim_search_result results[10];
        int nr = zim_search(h.z, title.c_str(), results, 10);
        int pick = -1;
        for (int i = 0; i < nr; i++) {
            if (results[i].title && strcasecmp(results[i].title, title.c_str()) == 0) {
                pick = i;
                break;
            }
        }
        if (pick < 0 && nr > 0) pick = 0;
        if (pick >= 0) found = zim_get_entry_by_index(h.z, results[pick].index, e);
        if (nr > 0) zim_search_free(results, nr);
    }
    if (found && e->is_redirect) zim_resolve_redirect(h.z, e);
    return found;
}

std::string entry_snippet(zim_archive * z, zim_entry * e, size_t max_len) {
    if (e->is_redirect) zim_resolve_redirect(z, e);
    size_t n = 0;
    char * txt = zim_get_content_text(z, e, &n);
    if (!txt) return std::string();
    std::string out = collapse_ws(txt, n);
    zim_free(txt);
    if (out.size() > max_len) out.resize(max_len);
    return out;
}

// ---------------------------------------------------------------------------
// HTTP response helpers + link rewriting (the /zim browser)
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

// zim_rewrite_link emits "/wiki/<path>" for internal targets; re-base that onto
// this ZIM's mount so links stay inside the right archive.
std::string rebase_link(const std::string & link, const std::string & id) {
    if (link.rfind("/wiki/", 0) == 0) {
        return "/zim/" + id + "/" + link.substr(6);
    }
    return link;
}

// Rewrite href/src attribute values: internal ZIM links -> /zim/<id>/<path>.
// External/anchor/absolute targets pass through (zim_rewrite_link decides).
std::string rewrite_html_links(const std::string & html, const std::string & id) {
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
                    out += rebase_link(rew.data(), id);
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

void inject_header_bar(std::string & html, const std::string & title, const std::string & id) {
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
        "<span style=\"margin-left:auto;opacity:.5;font-size:12px\">offline " +
        html_escape(id) + "</span>"
        "</div>";
    size_t b = html.find("<body");
    if (b == std::string::npos) { html.insert(0, bar); return; }
    size_t gt = html.find('>', b);
    if (gt == std::string::npos) { html.insert(0, bar); return; }
    html.insert(gt + 1, bar);
}

// Resolve a relative /zim path to a content entry (redirects followed). Empty
// path -> the archive's main page. Caller holds g_zim_mu.
bool resolve_path(ZimHandle & h, const std::string & rel, zim_entry * e) {
    bool found = false;
    if (rel.empty()) {
        found = zim_get_main_entry(h.z, e);
    } else if (zim_get_entry_by_path(h.z, rel.c_str(), e)) {
        found = true;
    } else {
        std::string c = "C/" + rel, a = "A/" + rel;
        if (zim_get_entry_by_path(h.z, c.c_str(), e) ||
            zim_get_entry_by_path(h.z, a.c_str(), e)) {
            found = true;
        }
    }
    if (found && e->is_redirect) zim_resolve_redirect(h.z, e);
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

// Render one entry of one ZIM to an HTTP response. Caller holds g_zim_mu.
server_http_res_ptr render_entry(ZimHandle & h, const std::string & rel) {
    zim_entry e;
    if (!resolve_path(h, rel, &e)) return not_found(rel);
    size_t n = 0;
    void * raw = zim_get_content(h.z, &e, &n);
    if (!raw) return not_found(rel);
    const char * mime = zim_get_mimetype(h.z, &e);
    bool is_html = mime && (strstr(mime, "text/html") || strstr(mime, "application/xhtml"));
    if (is_html) {
        std::string html((const char *) raw, n);
        zim_free(raw);
        std::string out = rewrite_html_links(html, h.id);
        inject_header_bar(out, e.title && *e.title ? e.title : rel, h.id);
        return serve_html(200, std::move(out));
    }
    std::string bytes((const char *) raw, n);
    zim_free(raw);
    return serve_raw(std::move(bytes), mime);
}

server_http_res_ptr handle_zim(const server_http_req & req) {
    // req.path is URL-decoded by the HTTP layer. Everything after "/zim/" is
    // "<id>/<rest>"; <rest> may itself contain '/'.
    std::string tail;
    size_t pos = req.path.find("/zim/");
    if (pos != std::string::npos) tail = req.path.substr(pos + 5);

    std::string id, rel;
    size_t slash = tail.find('/');
    if (slash == std::string::npos) { id = tail; rel = ""; }
    else { id = tail.substr(0, slash); rel = tail.substr(slash + 1); }

    std::lock_guard<std::mutex> lk(g_zim_mu);
    ensure_zims_open();
    ZimHandle * h = find_zim(id);
    if (!h) {
        return serve_html(404, "<!doctype html><meta charset=utf-8>"
                               "<body>no such ZIM id: <code>" + html_escape(id) +
                               "</code></body>");
    }
    return render_entry(*h, rel);
}

// Back-compat: /wiki/<path> aliases the FIRST opened ZIM.
server_http_res_ptr handle_wiki(const server_http_req & req) {
    std::string rel;
    size_t pos = req.path.find("/wiki/");
    if (pos != std::string::npos) rel = req.path.substr(pos + 6);

    std::lock_guard<std::mutex> lk(g_zim_mu);
    ensure_zims_open();
    if (g_zims.empty()) {
        return serve_html(503, "<!doctype html><meta charset=utf-8>"
                               "<body>wiki browser: no ZIM available.</body>");
    }
    return render_entry(g_zims.front(), rel);
}

// ---------------------------------------------------------------------------
// metadata (list_zims)
// ---------------------------------------------------------------------------

std::string zim_metadata(zim_archive * z, const char * key) {
    zim_entry e;
    std::string p = std::string("M/") + key;
    if (!zim_get_entry_by_path(z, p.c_str(), &e)) return "";
    size_t n = 0;
    void * c = zim_get_content(z, &e, &n);
    if (!c) return "";
    std::string s((const char *) c, n);
    zim_free(c);
    return s;
}

// article count: prefer the M/Counter "mime=count;..." breakdown (sum the
// html-ish entries — that's the article count), else the header entry count.
uint32_t zim_article_count(zim_archive * z) {
    std::string counter = zim_metadata(z, "Counter");
    uint64_t html = 0;
    bool any = false;
    size_t i = 0;
    while (i < counter.size()) {
        size_t semi = counter.find(';', i);
        std::string pair = counter.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string mime = pair.substr(0, eq);
            std::string cnt  = pair.substr(eq + 1);
            if (mime.find("html") != std::string::npos) {
                html += strtoull(cnt.c_str(), nullptr, 10);
                any = true;
            }
        }
        if (semi == std::string::npos) break;
        i = semi + 1;
    }
    return any ? (uint32_t) html : zim_get_entry_count(z);
}

// ---------------------------------------------------------------------------
// ranked search (the zim_search core)
// ---------------------------------------------------------------------------

struct Candidate {
    std::string zim;
    std::string title;
    std::string path;
    std::string snippet;
    double score = 0.0;
};

// Score weights: an exact (case-insensitive) title hit dominates so a one-word
// query surfaces its article; a prefix title hit still outranks fulltext; BM25
// mass (typically a few..tens) ranks the rest.
const double TITLE_EXACT_BOOST  = 1000.0;
const double TITLE_PREFIX_BOOST = 100.0;

// Search ONE zim, appending into `by_key` (keyed by "<id>\x1f<path>" so the
// same article from title + fulltext merges). Caller holds g_zim_mu.
void search_one(ZimHandle & h, const std::string & query, int limit,
                std::map<std::string, Candidate> & by_key) {
    ensure_xapian(h);

    auto bump = [&](const std::string & path, const std::string & title, double s) {
        std::string key = h.id + "\x1f" + path;
        auto it = by_key.find(key);
        if (it == by_key.end()) {
            Candidate c;
            c.zim = h.id; c.path = path; c.title = title; c.score = s;
            by_key.emplace(key, std::move(c));
        } else if (s > it->second.score) {
            it->second.score = s;
            if (it->second.title.empty()) it->second.title = title;
        }
    };

    // (a) title-listing matches (exact/prefix), boosted.
    {
        int cap = limit + 5;
        std::vector<zim_search_result> results(cap);
        int nr = zim_search(h.z, query.c_str(), results.data(), cap);
        for (int i = 0; i < nr; i++) {
            const char * t = results[i].title ? results[i].title : "";
            const char * p = results[i].path ? results[i].path : "";
            bool exact = (strcasecmp(t, query.c_str()) == 0);
            double s = (exact ? TITLE_EXACT_BOOST : TITLE_PREFIX_BOOST) + results[i].score;
            bump(p, t, s);
        }
        if (nr > 0) zim_search_free(results.data(), nr);
    }

    // (b) Xapian fulltext BM25 close-matches (if this ZIM has the index).
    if (h.fulltext) {
        zim_xapian_hit * hits = nullptr;
        int nh = zim_xapian_search(h.fulltext, query.c_str(), limit + 5, &hits);
        for (int i = 0; i < nh; i++) {
            const char * p = hits[i].path ? hits[i].path : "";
            const char * t = hits[i].title ? hits[i].title : p;
            bump(p, t, (double) hits[i].score);
        }
        if (nh > 0) zim_xapian_free_hits(hits, nh);
    }
}

// Decide whether an entry is a disambiguation page and, if so, extract its
// linked entries. Heuristic: title ends with "(disambiguation)", OR the lead
// text carries a disambiguation marker phrase ("may refer to", "can mean", ...).
// We deliberately do NOT treat "short + many links" as disambiguation — that
// false-fires on infobox-heavy stubs. Caller holds g_zim_mu.
bool extract_disambiguation(ZimHandle & h, const std::string & path,
                            const std::string & title,
                            std::vector<std::pair<std::string, std::string>> & links_out) {
    bool title_says = title.size() >= 16 &&
        !strcasecmp(title.c_str() + title.size() - 16, "(disambiguation)");

    zim_entry e;
    if (!resolve_in_zim(h, path.empty() ? title : path, &e)) return false;
    size_t n = 0;
    char * md = zim_get_content_markdown(h.z, &e, &n);
    if (!md) return false;
    std::string body(md, n);
    zim_free(md);

    // disambiguation marker phrase in the lead (first ~800 chars, lowercased)
    bool phrase = false;
    {
        std::string lead = body.substr(0, std::min<size_t>(body.size(), 800));
        for (auto & c : lead) c = (char) tolower((unsigned char) c);
        static const char * markers[] = {
            "may refer to", "can refer to", "may mean", "can mean",
            "may stand for", "can mean many things", "most often refers to",
        };
        for (const char * m : markers) {
            if (lead.find(m) != std::string::npos) { phrase = true; break; }
        }
    }
    if (!title_says && !phrase) return false;

    // pull "[text](/zim/<id>/path)" and "[text](/wiki/path)" link pairs
    std::vector<std::pair<std::string, std::string>> links;
    size_t i = 0;
    while ((i = body.find("](", i)) != std::string::npos) {
        // back up to the matching '['
        size_t open = body.rfind('[', i);
        if (open == std::string::npos) { i += 2; continue; }
        std::string text = body.substr(open + 1, i - open - 1);
        size_t url_start = i + 2;
        size_t url_end = body.find(')', url_start);
        if (url_end == std::string::npos) break;
        std::string url = body.substr(url_start, url_end - url_start);
        i = url_end + 1;
        std::string lpath;
        if (url.rfind("/zim/", 0) == 0) {
            size_t s = url.find('/', 5);
            if (s != std::string::npos) lpath = url.substr(s + 1);
        } else if (url.rfind("/wiki/", 0) == 0) {
            lpath = url.substr(6);
        } else {
            continue;  // external/anchor
        }
        if (lpath.empty() || text.empty()) continue;
        links.emplace_back(text, lpath);
    }

    // de-dup, cap
    std::map<std::string, bool> seen;
    for (auto & l : links) {
        if (seen.count(l.second)) continue;
        seen[l.second] = true;
        links_out.push_back(l);
        if (links_out.size() >= 25) break;
    }
    return !links_out.empty();
}

// ---------------------------------------------------------------------------
// the in-process tools
// ---------------------------------------------------------------------------

struct ZimSearchTool : server_tool {
    ZimSearchTool() { name = "zim_search"; display_name = name; permission_write = false; }

    json get_definition() override {
        return {
            {"type", "function"},
            {"function", json{
                {"name", name},
                {"description",
                 "Ranked full-text search over the bundled offline ZIM archive(s) "
                 "(e.g. Wikipedia). Uses the ZIM's own Xapian BM25 index, so it "
                 "returns the CLOSEST-matching articles even when not every query "
                 "word matches — query the topic/keywords or a natural-language "
                 "question (e.g. \"TNT inventor\", \"largest cities in the world\"). "
                 "Exact title matches are boosted. Returns "
                 "{matches:[{zim,title,path,snippet,score}], disambiguation?}, each "
                 "result tagged with its source `zim`. Call list_zims to see what is "
                 "available. Pass a returned title/path to zim_get_article to read "
                 "the full article, or zim_open for a clickable link."},
                {"parameters", json{
                    {"type", "object"},
                    {"properties", json{
                        {"query", json{{"type", "string"},
                                       {"description", "Topic, keywords or a natural-language question."}}},
                        {"zim",   json{{"type", "string"},
                                       {"description", "Optional: restrict to one ZIM by its id (see list_zims)."}}},
                        {"limit", json{{"type", "integer"},
                                       {"description", "Max results (default 5, max 50)."}}},
                    }},
                    {"required", json::array({"query"})},
                }},
            }},
        };
    }

    json invoke(json params) override {
        if (!params.is_object() || !params.contains("query") || !params["query"].is_string()) {
            return {{"error", "missing required string argument 'query'"}};
        }
        std::string query = params["query"].get<std::string>();
        std::string only;
        if (params.contains("zim") && params["zim"].is_string()) only = params["zim"].get<std::string>();
        int limit = 5;
        if (params.contains("limit") && params["limit"].is_number_integer()) limit = params["limit"].get<int>();
        if (limit < 1) limit = 1;
        if (limit > 50) limit = 50;

        std::lock_guard<std::mutex> lk(g_zim_mu);
        ensure_zims_open();
        if (g_zims.empty()) return {{"error", "no ZIM archive is open"}};

        std::map<std::string, Candidate> by_key;
        for (auto & h : g_zims) {
            if (!only.empty() && h.id != only) continue;
            search_one(h, query, limit, by_key);
        }
        if (!only.empty() && !find_zim(only)) {
            return {{"error", "no such ZIM id: " + only}};
        }

        std::vector<Candidate> all;
        all.reserve(by_key.size());
        for (auto & kv : by_key) all.push_back(std::move(kv.second));
        std::sort(all.begin(), all.end(),
                  [](const Candidate & a, const Candidate & b) { return a.score > b.score; });
        if ((int) all.size() > limit) all.resize(limit);

        // snippets (resolve each path in its zim)
        for (auto & c : all) {
            ZimHandle * h = find_zim(c.zim);
            if (!h) continue;
            zim_entry e;
            if (resolve_in_zim(*h, c.path.empty() ? c.title : c.path, &e)) {
                c.snippet = entry_snippet(h->z, &e, 200);
                if (c.title.empty() && e.title) c.title = e.title;
            }
        }

        json matches = json::array();
        for (auto & c : all) {
            matches.push_back(json{
                {"zim", c.zim},
                {"title", c.title},
                {"path", c.path},
                {"snippet", c.snippet},
                {"score", c.score},
            });
        }

        json result = json::object();
        result["matches"] = matches;

        // disambiguation inlining for the top hit
        if (!all.empty()) {
            ZimHandle * h = find_zim(all.front().zim);
            if (h) {
                std::vector<std::pair<std::string, std::string>> links;
                if (extract_disambiguation(*h, all.front().path, all.front().title, links)) {
                    json opts = json::array();
                    for (auto & l : links) {
                        opts.push_back(json{{"title", l.first}, {"path", l.second}});
                    }
                    result["disambiguation"] = json{{all.front().title, opts}};
                }
            }
        }
        return result;
    }
};

struct ZimGetArticleTool : server_tool {
    ZimGetArticleTool() { name = "zim_get_article"; display_name = name; permission_write = false; }

    json get_definition() override {
        return {
            {"type", "function"},
            {"function", json{
                {"name", name},
                {"description",
                 "Fetch a full article from a bundled offline ZIM, by title or path "
                 "(as returned by zim_search). Returns clean Markdown by default "
                 "(headings, lists, bold/italic and [links](/zim/<id>/...) you can "
                 "click through), or pass format:\"text\" for cheaper flattened plain "
                 "text. Redirects are resolved automatically."},
                {"parameters", json{
                    {"type", "object"},
                    {"properties", json{
                        {"title",  json{{"type", "string"},
                                        {"description", "Article title or path (e.g. \"Eiffel Tower\")."}}},
                        {"zim",    json{{"type", "string"},
                                        {"description", "Optional: which ZIM to read from (id, see list_zims). Default: search all."}}},
                        {"format", json{{"type", "string"},
                                        {"enum", json::array({"markdown", "text"})},
                                        {"description", "\"markdown\" (default) or \"text\"."}}},
                    }},
                    {"required", json::array({"title"})},
                }},
            }},
        };
    }

    json invoke(json params) override {
        std::string title;
        if (params.is_object() && params.contains("title") && params["title"].is_string())
            title = params["title"].get<std::string>();
        if (title.empty()) return {{"error", "missing required string argument 'title'"}};
        std::string only, format = "markdown";
        if (params.contains("zim") && params["zim"].is_string()) only = params["zim"].get<std::string>();
        if (params.contains("format") && params["format"].is_string()) format = params["format"].get<std::string>();

        std::lock_guard<std::mutex> lk(g_zim_mu);
        ensure_zims_open();
        if (g_zims.empty()) return {{"error", "no ZIM archive is open"}};

        for (auto & h : g_zims) {
            if (!only.empty() && h.id != only) continue;
            zim_entry e;
            if (!resolve_in_zim(h, title, &e)) continue;
            size_t n = 0;
            char * body = (format == "text")
                ? zim_get_content_text(h.z, &e, &n)
                : zim_get_content_markdown(h.z, &e, &n);
            if (!body || n == 0) { if (body) zim_free(body); continue; }
            std::string out(body, n);
            zim_free(body);
            // the shared Markdown converter emits "](/wiki/...)"; rebase onto
            // THIS zim's mount so links resolve to the right archive.
            if (format != "text") {
                const std::string from = "](/wiki/", to = "](/zim/" + h.id + "/";
                for (size_t p = 0; (p = out.find(from, p)) != std::string::npos; p += to.size())
                    out.replace(p, from.size(), to);
            }
            return {{"plain_text_response", out}};
        }
        return {{"error", "article not found: " + title}};
    }
};

struct ZimOpenTool : server_tool {
    ZimOpenTool() { name = "zim_open"; display_name = name; permission_write = false; }

    json get_definition() override {
        return {
            {"type", "function"},
            {"function", json{
                {"name", name},
                {"description",
                 "Resolve an article (by title or path) to a clickable URL into the "
                 "offline encyclopedia browser served by this llamafile. Returns "
                 "{url, zim, title, path} where url is \"/zim/<id>/<path>\" — hand it "
                 "to the user so they can open the rendered article and click "
                 "through internal links. Redirects are resolved automatically."},
                {"parameters", json{
                    {"type", "object"},
                    {"properties", json{
                        {"title", json{{"type", "string"},
                                       {"description", "Article title (e.g. \"Eiffel Tower\")."}}},
                        {"path",  json{{"type", "string"},
                                       {"description", "Article path (alternative to title)."}}},
                        {"zim",   json{{"type", "string"},
                                       {"description", "Optional: which ZIM (id, see list_zims). Default: first match."}}},
                    }},
                }},
            }},
        };
    }

    json invoke(json params) override {
        std::string title;
        if (params.is_object()) {
            if (params.contains("title") && params["title"].is_string()) title = params["title"].get<std::string>();
            else if (params.contains("path") && params["path"].is_string()) title = params["path"].get<std::string>();
        }
        if (title.empty()) return {{"error", "provide a string 'title' or 'path'"}};
        std::string only;
        if (params.contains("zim") && params["zim"].is_string()) only = params["zim"].get<std::string>();

        std::lock_guard<std::mutex> lk(g_zim_mu);
        ensure_zims_open();
        if (g_zims.empty()) return {{"error", "no ZIM archive is open"}};

        for (auto & h : g_zims) {
            if (!only.empty() && h.id != only) continue;
            zim_entry e;
            if (!resolve_in_zim(h, title, &e)) continue;
            std::string path = e.path ? e.path : title;
            return json{
                {"url", "/zim/" + h.id + "/" + url_encode_path(path.c_str())},
                {"zim", h.id},
                {"title", e.title ? e.title : path},
                {"path", path},
            };
        }
        return {{"error", "article not found: " + title}};
    }
};

struct ListZimsTool : server_tool {
    ListZimsTool() { name = "list_zims"; display_name = name; permission_write = false; }

    json get_definition() override {
        return {
            {"type", "function"},
            {"function", json{
                {"name", name},
                {"description",
                 "List the offline ZIM archives bundled with this llamafile. Returns "
                 "[{id, title, language, article_count}]. Use an `id` to scope "
                 "zim_search / zim_get_article / zim_open to one archive."},
                {"parameters", json{{"type", "object"}, {"properties", json::object()}}},
            }},
        };
    }

    json invoke(json) override {
        std::lock_guard<std::mutex> lk(g_zim_mu);
        ensure_zims_open();
        json arr = json::array();
        for (auto & h : g_zims) {
            std::string t = zim_metadata(h.z, "Title");
            std::string lang = zim_metadata(h.z, "Language");
            arr.push_back(json{
                {"id", h.id},
                {"title", t.empty() ? h.id : t},
                {"language", lang},
                {"article_count", zim_article_count(h.z)},
            });
        }
        return {{"zims", arr}};
    }
};

// ---------------------------------------------------------------------------
// startup warm-up
// ---------------------------------------------------------------------------
//
// The 21 ZIM Xapian indexes fault in lazily from the (often /zip-backed,
// possibly external-disk) bundle the FIRST time a query touches them, so a cold
// first `zim_search` pays ~30s of B-tree-root + hot-block page faults. This
// detached background thread does that faulting AHEAD of the first real user
// query: for every registered ZIM it runs the same ensure_xapian() open path,
// then a trivial throwaway query ("the", limit 1) against the fulltext + title
// Glass indexes so their B-tree roots and hot postlist blocks are resident. The
// first real search then hits warm indexes.
//
// It grabs g_zim_mu PER ZIM (released between archives) because the zim reader's
// cluster cache + the Xapian readers are not thread-safe. A live search arriving
// mid-warm therefore interleaves between archives rather than waiting for the
// whole sweep — though, by necessity, it still serializes with whichever single
// ZIM is being faulted at that instant (see the report's caveat).

// Fault one Glass index's hot blocks with a throwaway BM25 query. Caller holds
// g_zim_mu (the Xapian readers are not thread-safe).
void warm_xapian(zim_xapian * idx) {
    if (!idx) return;
    zim_xapian_hit * hits = nullptr;
    int nh = zim_xapian_search(idx, "the", 1, &hits);
    if (nh > 0) zim_xapian_free_hits(hits, nh);
}

void * wiki_warm_thread_fn(void *) {
    // Let startup logs settle and the model/HTTP bring-up finish its own I/O
    // before we start faulting indexes (this thread is fully detached, so the
    // delay never holds anything up).
    usleep(1500 * 1000);

    size_t n;
    {
        std::lock_guard<std::mutex> lk(g_zim_mu);
        ensure_zims_open();
        n = g_zims.size();
    }
    if (n == 0) return nullptr;

    fprintf(stderr, "wiki: warming %zu ZIM index%s in the background…\n",
            n, n == 1 ? "" : "es");
    auto t_all0 = std::chrono::steady_clock::now();

    for (size_t i = 0; i < n; i++) {
        auto t0 = std::chrono::steady_clock::now();
        std::string id;
        bool had_ft = false;
        try {
            // Per-ZIM lock: released before the next archive so a concurrent
            // user search can slip in between archives instead of waiting out
            // the whole sweep.
            std::lock_guard<std::mutex> lk(g_zim_mu);
            if (i >= g_zims.size()) break;
            ZimHandle & h = g_zims[i];
            id = h.id;
            ensure_xapian(h);            // open X/fulltext + X/title (+ stemmer)
            had_ft = (h.fulltext != nullptr);
            warm_xapian(h.fulltext);     // fault the fulltext Glass B-tree
            warm_xapian(h.title);        // fault the title Glass B-tree
            // also exercise the reader's own title-listing search path
            zim_search_result r[1];
            int nr = zim_search(h.z, "the", r, 1);
            if (nr > 0) zim_search_free(r, nr);
        } catch (const std::exception & e) {
            fprintf(stderr, "wiki: warm[%s] error: %s\n",
                    id.empty() ? "?" : id.c_str(), e.what());
            continue;
        } catch (...) {
            fprintf(stderr, "wiki: warm[%s] error: unknown exception\n",
                    id.empty() ? "?" : id.c_str());
            continue;
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        fprintf(stderr, "wiki: warm[%s] %lldms%s\n", id.c_str(), (long long) ms,
                had_ft ? "" : " (no fulltext index — skipped)");
    }

    double secs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t_all0).count() / 1000.0;
    fprintf(stderr, "wiki: all %zu ZIM index%s warm in %.1fs\n",
            n, n == 1 ? "" : "es", secs);
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
void llamafile_wiki_enable(const char * zim_path) {
    if (zim_path && *zim_path) {
        std::lock_guard<std::mutex> lk(g_zim_mu);
        g_zim_paths.push_back(zim_path);
    }
    g_wiki_enabled = true;
}

bool llamafile_wiki_enabled() {
    return g_wiki_enabled;
}

int llamafile_wiki_register_tools(server_tools & registry) {
    if (!g_wiki_enabled) return 0;
    {
        std::lock_guard<std::mutex> lk(g_zim_mu);
        ensure_zims_open();  // surface a bad path at startup
    }
    registry.tools.push_back(std::make_unique<ZimSearchTool>());
    registry.tools.push_back(std::make_unique<ZimGetArticleTool>());
    registry.tools.push_back(std::make_unique<ZimOpenTool>());
    registry.tools.push_back(std::make_unique<ListZimsTool>());
    return 4;
}

void llamafile_wiki_warm();  // fwd decl (defined just below)

void llamafile_wiki_register_routes(server_http_context & http) {
    {
        std::lock_guard<std::mutex> lk(g_zim_mu);
        ensure_zims_open();  // open now so a bad path is reported at startup
    }
    // regex routes ("(.*)" captures sub-paths containing '/').
    http.get("/zim/(.*)", handle_zim);
    http.get("/wiki/(.*)", handle_wiki);  // back-compat alias -> first ZIM
    llamafile_wiki_warm();  // start the background index warm-up (detached)
}

// Kick off the background index warm-up (no-op if nothing was registered). Runs
// on a DETACHED thread so it never delays startup, /health, or the first
// request; called from the server-ready seam (llamafile_wiki_register_routes,
// which server.cpp invokes as the HTTP server comes up). An explicit 8 MiB stack
// matches the other llamafile loopback/boot workers (Cosmopolitan's default
// thread stack is too small for the Xapian + nlohmann call chains).
void llamafile_wiki_warm() {
    {
        std::lock_guard<std::mutex> lk(g_zim_mu);
        if (g_zim_paths.empty()) return;  // no --zim => nothing to warm
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    if (pthread_create(&tid, &attr, wiki_warm_thread_fn, nullptr) != 0) {
        fprintf(stderr, "wiki: failed to spawn warm-up thread\n");
    }
    pthread_attr_destroy(&attr);
}
