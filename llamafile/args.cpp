// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2024 Mozilla Foundation
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

#include "args.h"
#include "llamafile.h"
#include "mcp_host.h"
#include "agent_loop.h"
#include "webcam_agent.h"

// llamafile interactive runtime (agent_runtime_server.cpp) — session root setter
// for the --session-dir flag (cross-TU, same pattern as the other hooks).
void llamafile_runtime_set_session_root(const char * dir);

#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// ---------------------------------------------------------------------------
// File-tool path jail root (SECURITY)
//
// The built-in server file tools (read_file/write_file/edit_file/apply_diff/
// file_glob_search/grep_search in llama.cpp/tools/server/server-tools.cpp)
// confine every path they touch to this root directory. It defaults to the
// server's current working directory and is overridable via --tools-root DIR.
//
// These two symbols live in the global namespace so server-tools.cpp can call
// llamafile_tools_root() across the llamafile<->llama.cpp link boundary (the
// same pattern the multi-agent hooks use, see server.cpp).
// ---------------------------------------------------------------------------
static std::string g_tools_root;

void llamafile_set_tools_root(const char * dir) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(std::filesystem::path(dir), ec);
    if (ec || p.empty()) {
        p = std::filesystem::absolute(std::filesystem::path(dir), ec);
    }
    g_tools_root = p.empty() ? std::string(dir) : p.string();
}

std::string llamafile_tools_root() {
    if (g_tools_root.empty()) {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::current_path(ec);
        g_tools_root = ec ? std::string(".") : p.string();
    }
    return g_tools_root;
}

namespace lf {

// Static storage for filtered argv (persists after function returns)
static std::vector<char*> g_filtered_argv;

// Helper: returns true if arg is a llamafile-specific flag (not recognized by llama.cpp)
static bool is_llamafile_flag(const char* arg) {
    return strcmp(arg, "--server") == 0 ||
           strcmp(arg, "--chat") == 0 ||
           strcmp(arg, "--cli") == 0 ||
           strcmp(arg, "--gpu") == 0 ||
           strcmp(arg, "--ascii") == 0 ||
           strcmp(arg, "--nologo") == 0 ||
           strcmp(arg, "--nothink") == 0 ||
           strcmp(arg, "--webcam-agent") == 0 ||
           strcmp(arg, "--version") == 0;
}

LlamafileArgs parse_llamafile_args(int argc, char** argv) {
    LlamafileArgs args;

    // Lock in the file-tool jail root to the startup cwd (unless --tools-root
    // overrides it below). Captured early so a later chdir can't widen it.
    (void) ::llamafile_tools_root();

    // Early GPU init must happen before we filter args
    // This reads --gpu and -ngl flags to set FLAG_gpu
    llamafile_early_gpu_init(argv);

    // Capture -p/--prompt value before filtering (needed for combined mode
    // where SERVER parsing excludes -p)
    // Note: Loop does not break early; if multiple -p flags are given,
    // the last occurrence wins (intentional for override flexibility)
    for (int i = 0; i < argc; ++i) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--prompt") == 0) && i + 1 < argc) {
            args.system_prompt = argv[i + 1];
        }
        if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--model") == 0) && i + 1 < argc) {
            args.model_path = argv[i + 1];
        }
    }

    // Determine execution mode from flags
    // Priority: explicit flags override defaults
    if (llamafile_has(argv, "--server")) {
        args.mode = ProgramMode::SERVER;
    } else if (llamafile_has(argv, "--chat")) {
        args.mode = ProgramMode::CHAT;
    } else if (llamafile_has(argv, "--cli")) {
        args.mode = ProgramMode::CLI;
    } else {
        // AUTO mode: will run combined chat + server
        args.mode = ProgramMode::AUTO;
    }

    // Check verbose flag
    FLAG_verbose = llamafile_has(argv, "--verbose") ? 1 : 0;

    // Check --nothink flag (filters thinking/reasoning content in CLI mode)
    FLAG_nothink = llamafile_has(argv, "--nothink");

    // Check logo flags
    FLAG_nologo = llamafile_has(argv, "--nologo");
    FLAG_ascii = llamafile_has(argv, "--ascii");

    // Filter out llamafile-specific arguments
    // These are not recognized by llama.cpp and would cause errors
    g_filtered_argv.clear();

    for (int i = 0; i < argc; ++i) {
        const char* arg = argv[i];

        // --mcp '<command line>': configure an external MCP server to spawn and
        // bridge into the /tools registry (llamafile is the MCP HOST). Repeatable.
        // Consume the flag + its value so they never reach llama.cpp's parser.
        if (strcmp(arg, "--mcp") == 0) {
            if (i + 1 < argc) {
                llamafile_mcp_add_server(argv[i + 1]);
                ++i;
            }
            continue;
        }

        // --wikidata PATH: enable the offline Wikidata fact tools (wikidata_*)
        // in --server mode by bridging them in via our own mcp-server subprocess
        // (the same path as a manual `--mcp 'llamafile mcp-server --wikidata …'`),
        // so they land in the /tools registry alongside the wiki_* tools.
        // Consumed here so it never reaches llama.cpp's parser.
        if (strcmp(arg, "--wikidata") == 0) {
            if (i + 1 < argc) {
                std::string self = (argc > 0 && argv[0]) ? argv[0] : "llamafile";
                std::string cmd = "'" + self + "' mcp-server --wikidata '" +
                                  std::string(argv[i + 1]) + "'";
                llamafile_mcp_add_server(cmd);
                ++i;
            }
            continue;
        }

        // --wiki-fts PATH: enable the offline Wikipedia full-text tool
        // (wiki_fulltext_search, article bodies) in --server mode by bridging it
        // in via our own mcp-server subprocess (same path as a manual
        // `--mcp 'llamafile mcp-server --wiki-fts …'`), so it lands in the /tools
        // registry alongside the wiki_* title tools. Consumed here so it never
        // reaches llama.cpp's parser.
        if (strcmp(arg, "--wiki-fts") == 0) {
            if (i + 1 < argc) {
                std::string self = (argc > 0 && argv[0]) ? argv[0] : "llamafile";
                std::string cmd = "'" + self + "' mcp-server --wiki-fts '" +
                                  std::string(argv[i + 1]) + "'";
                llamafile_mcp_add_server(cmd);
                ++i;
            }
            continue;
        }

        // --tools-root DIR: confine the built-in server file tools to DIR
        // (default: the server's cwd). llamafile-owned security flag; consumed
        // here so it never reaches llama.cpp's parser.
        if (strcmp(arg, "--tools-root") == 0) {
            if (i + 1 < argc) {
                llamafile_set_tools_root(argv[i + 1]);
                ++i;
            }
            continue;
        }

        // --agents: register the delegate_to_<role> sub-agent tools (multi-agent
        // orchestration via sub-agents-as-tools). llamafile-owned flag; consumed
        // here so it never reaches llama.cpp's parser.
        if (strcmp(arg, "--agents") == 0) {
            llamafile_agents_enable();
            continue;
        }

        // --session-dir DIR: root for the interactive runtime's persisted
        // sessions (Phase 4). llamafile-owned flag; consumed here so it never
        // reaches llama.cpp's parser.
        if (strcmp(arg, "--session-dir") == 0) {
            if (i + 1 < argc) {
                llamafile_runtime_set_session_root(argv[i + 1]);
                ++i;
            }
            continue;
        }

        // --webcam-agent: enable the server "webcam agent" mode (/agent/*
        // HTTP+SSE endpoints driving a vlib-video continuous session, step 2).
        // llamafile-owned flag; consumed here so it never reaches llama.cpp.
        if (strcmp(arg, "--webcam-agent") == 0) {
            llamafile_webcam_enable();
            continue;
        }

        // Skip llamafile-specific flags
        if (is_llamafile_flag(arg)) {
            // --gpu takes a value argument, skip it too
            if (strcmp(arg, "--gpu") == 0 && i + 1 < argc) {
                ++i;
            }
            continue;
        }

        // Keep this argument
        g_filtered_argv.push_back(argv[i]);
    }

    // Null-terminate argv array (required by convention)
    g_filtered_argv.push_back(nullptr);

    args.llama_argc = static_cast<int>(g_filtered_argv.size()) - 1;
    args.llama_argv = g_filtered_argv.data();

    return args;
}

} // namespace lf
