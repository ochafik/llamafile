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

// llamafile WIKI BROWSER — an in-process offline Wikipedia browser.
//
// When `--zim PATH` is given to `--server`, the main server process opens the
// ZIM in-process (in ADDITION to the mcp-server subprocess bridge that backs
// the wiki_* tools) and serves the article HTML straight from the archive:
//
//   GET /wiki/<path>   -> the article's ORIGINAL HTML, with internal links
//                         rewritten to /wiki/... so you can click through the
//                         encyclopedia inside the binary. Non-HTML entries
//                         (CSS/JS/images) are streamed with their ZIM MIME type
//                         so the page renders with its real styling. 404 if
//                         the path is absent.
//
// The companion `wiki_open(title|path) -> {url}` tool lives in the mcp-server
// subprocess (mcp_server.cpp); it hands the agent a /wiki/<path> link that this
// route serves.

struct server_http_context;  // llama.cpp/tools/server/server-http.h

// `--zim PATH` plumbing (called during llamafile arg parsing in args.cpp): open
// the ZIM in-process for the /wiki article browser. Safe to call more than once
// (last path wins); the archive is opened lazily at route-registration time.
void llamafile_wiki_enable(const char * zim_path);
bool llamafile_wiki_enabled();

// Register the GET /wiki/<path> route on the server's HTTP context. Called from
// the server.cpp seam BEFORE the HTTP server starts. Opens the in-process ZIM
// handle if not already open; a failed open disables the route (logged, not
// fatal).
void llamafile_wiki_register_routes(server_http_context & http);
