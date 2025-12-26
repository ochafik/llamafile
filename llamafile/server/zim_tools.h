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

#pragma once
#include <string>
#include <optional>

namespace lf {
namespace server {

// Result of parsing tool invocation patterns
struct ToolInvocation {
    enum class Type { NONE, SEARCH, READ };
    Type type = Type::NONE;
    std::string argument;
    size_t start_pos = 0;
    size_t end_pos = 0;
};

// Check if ZIM tools are available and enabled
bool zim_tools_available();

// Get the system prompt addition for Wikipedia tools
std::string get_zim_tools_system_prompt();

// Detect a tool invocation pattern in the given text
// Looks for [SEARCH: query] or [READ: path] patterns
ToolInvocation detect_tool_invocation(const std::string& text);

// Execute a tool invocation and return the result
// Returns nullopt if execution fails
std::optional<std::string> execute_tool(const ToolInvocation& invocation);

} // namespace server
} // namespace lf
