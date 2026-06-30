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

#pragma once

// llamafile ZIM REGISTRY — an in-process offline encyclopedia (one or more ZIM
// archives) with a browser AND the search tools, sharing one set of handles.
//
// When `--zim PATH` is given to `--server` (REPEATABLE), the main server opens
// each ZIM + its embedded Xapian BM25 indexes in-process and serves:
//
//   GET /zim/<id>/<path> -> the article's ORIGINAL HTML, internal links
//                           rewritten to /zim/<id>/... so you can click through
//                           the offline encyclopedia inside the binary. Non-HTML
//                           entries (CSS/JS/images) stream with their ZIM MIME
//                           type. `id` is the ZIM's filename stem (see list_zims).
//   GET /wiki/<path>     -> back-compat alias to the FIRST ZIM.
//
// and the in-process tools zim_search / zim_get_article / zim_open / list_zims
// (see llamafile_wiki_register_tools), which the agentic loop and the model call
// over the same shared handles.

struct server_http_context;  // llama.cpp/tools/server/server-http.h
struct server_tools;         // llama.cpp/tools/server/server-tools.h

// `--zim PATH` plumbing (called during llamafile arg parsing in args.cpp): add
// a ZIM to the in-process registry for the /zim article browser AND the
// in-process zim_* tools. REPEATABLE — each call appends one archive (multiple
// --zim flags => multiple archives, each addressable by its filename-stem id).
// The archives are opened lazily at route/tool-registration time.
void llamafile_wiki_enable(const char * zim_path);
bool llamafile_wiki_enabled();

// Register the in-process ZIM tools (zim_search, zim_get_article, zim_open,
// list_zims) into the main server's tool registry, sharing the same open ZIM +
// Xapian handles as the /zim route. Returns the number of tools registered.
// Called from llamafile_mcp_register_tools (mcp_host.cpp) so it lands in /tools
// BEFORE the delegate/runtime roles are registered (their allow-lists can see
// it). No-op (returns 0) unless --zim was given.
int llamafile_wiki_register_tools(server_tools & registry);

// Register the GET /zim/<id>/<path> route (and the back-compat /wiki/<path>
// alias to the first ZIM) on the server's HTTP context. Called from the
// server.cpp seam BEFORE the HTTP server starts. Opens the in-process ZIM
// handles if not already open; a failed open is logged, not fatal.
void llamafile_wiki_register_routes(server_http_context & http);

// Kick off a NON-BLOCKING background warm-up of every registered ZIM's Xapian
// indexes (fulltext + title), so the FIRST zim_search hits warm B-trees instead
// of paying ~30s of lazy page-faults from the (often external-disk / /zip-backed)
// bundle. Runs on a detached thread; no-op unless --zim was given. Spawned from
// llamafile_wiki_register_routes as the server comes up.
void llamafile_wiki_warm();
