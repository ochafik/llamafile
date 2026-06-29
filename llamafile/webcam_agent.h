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

#include <functional>

// llamafile WEBCAM AGENT — server "webcam-agent" mode (video VLM, step 2).
//
// Exposes a small set of HTTP+SSE endpoints (NO WebSocket — cosmocc gotcha)
// that drive a single vlib-video continuous session bound to one dedicated
// llama_context/slot, and runs the per-frame agentic loop: when the model
// witnesses something matching its watch instructions it emits a tool_call
// (e.g. send_email) which is forwarded to a connected MCP server (via the
// existing mcp_host) and the result is fed back into the session.
//
//   POST /agent/start   {prompt, frame_size?}  -> create+start the session
//   POST /agent/frame   (JPEG bytes)           -> decode -> process_frame ->
//                                                 ACTION_OTHER -> MCP -> loop
//   GET  /agent/events  (SSE)                  -> per-frame action/note/speak/
//                                                 tool_call/tool_result stream
//   POST /agent/stop                           -> destroy session, free ctx
//   POST /agent/clip    (501; step 3)          -> 30s-buffer email attachment
//   POST /agent/live    (501; step 3)          -> live-link relay
//
// See /Users/ochafik/github/llama.cpp-video-ddocs/04-server-agent-mode-and-protocol.md
// The session itself is llamafile/vlib_video/vlib_video_session.{h,cpp} (step 1).

struct server_http_context;  // llama.cpp/tools/server/server-http.h
struct common_params;        // llama.cpp/common/common.h
struct llama_context;        // llama.cpp/include/llama.h

// `--webcam-agent` flag plumbing (set during llamafile arg parsing in args.cpp).
void llamafile_webcam_enable();
bool llamafile_webcam_enabled();

// Register the /agent/* routes on the server's HTTP context. Called from the
// server.cpp seam BEFORE the HTTP server starts (routes must be registered up
// front), capturing a getter for the server's live llama_context so the model
// (loaded later, after the HTTP server is already serving /health) can be
// reached lazily at request time. `params` is copied so the session can build
// its own dedicated context + mtmd from the same model + mmproj.
void llamafile_webcam_register_routes(server_http_context & http,
                                      const common_params & params,
                                      std::function<llama_context *()> get_server_ctx);
