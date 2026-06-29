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

#include <string>
#include <vector>

// llamafile MULTI-AGENT ORCHESTRATION — "sub-agents-as-tools".
//
// Each sub-agent role (researcher / ideator / verifier) is registered as a
// `delegate_to_<role>` tool in the SAME `/tools` registry the MCP host uses.
// Invoking such a tool runs that role's OWN server-side agentic loop
// (`llamafile_run_agent`): it POSTs to the server's own
// `http://<host>:<port>/v1/chat/completions` with the role's allow-listed tool
// subset, dispatches the model's tool_calls back to the server's `POST /tools`,
// loops until `finish_reason != tool_calls` (or a hard turn cap), and returns
// the final distilled assistant text.
//
// Because the delegate tools live in the ordinary tool registry, llamafile's
// EXISTING agentic loop (web UI / CLI / any OpenAI client) becomes a
// multi-agent ORCHESTRATOR for free: its toolset now includes sub-agents. No
// frontend change. See ddocs/02-multiagent-webui-design.md and the validated
// reference ddocs/prototypes/orchestrate.py.
//
// This is llamafile-owned and recurses the loop that already exists, exactly
// as the design specifies (agent-as-tool), but server-side instead of
// client-side so every client benefits.

struct server_tools;  // llama.cpp/tools/server/server-tools.h
namespace agentrt { struct EventBroker; }  // agent_runtime.h (SSE broker)

// Configure the loopback endpoint the sub-agent loop POSTs to. Called from the
// server once the bind address is known (server.cpp). `api_key` may be empty;
// `model` is the alias to put in the request body (the server is single-model
// so any non-empty value works). `n_parallel` is the server slot budget,
// recorded for diagnostics / soft fan-out advice.
void llamafile_agents_set_endpoint(const std::string & host, int port,
                                   const std::string & api_key,
                                   const std::string & model,
                                   int n_parallel);

// `--agents` flag plumbing (set during llamafile arg parsing in args.cpp).
void llamafile_agents_enable();
bool llamafile_agents_enabled();

// Register delegate_to_researcher / delegate_to_ideator / delegate_to_verifier
// into `registry`. Each delegate's allow-list is intersected with the tools
// ACTUALLY present in the registry (built-ins + MCP-bridged), so a role only
// advertises tools that exist. Returns the number of delegate tools added.
// Call AFTER built-in + MCP tools are registered.
int llamafile_agents_register_tools(server_tools & registry);

// The reusable server-side agentic-loop runner used by every delegate tool.
// Builds [system, user] messages, advertises the allow-listed tools, POSTs to
// the local /v1/chat/completions, dispatches tool_calls (concurrently within a
// turn) to /tools, and loops with a hard `max_turns` cap. `depth` guards
// against unbounded recursion. Returns the final assistant text (the distilled
// result), or a "(stopped: ...)" marker on a cap/error.
//
// `role` is a short label (researcher/ideator/verifier) used to TAG the LIVE
// activity events streamed over the delegate-activity broker (see below) so the
// /agents UI can attribute each step to its sub-agent. Pass "" for an untagged
// run.
std::string llamafile_run_agent(const std::string & role,
                                const std::string & system_prompt,
                                const std::string & user_task,
                                const std::vector<std::string> & tool_allowlist,
                                int max_turns,
                                int depth);

// LIVE delegate-activity SSE broker. Every `delegate_to_*` invocation streams
// structured JSON events here as it runs (start / turn / tool_call / tool_result
// / final / maxturns / error), so the /agents UI can WATCH a synchronous
// sub-agent work in real time. Returns a process-global broker (never null);
// the /agents/activity SSE endpoint tails it. See agent_loop.cpp.
agentrt::EventBroker * llamafile_agents_activity_broker();
