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

#include "zim_tools.h"
#include <cassert>
#include <iostream>

using namespace lf::server;

static void test_detect_search_pattern() {
    std::string text = "Let me search for that. [SEARCH: Albert Einstein] ...";
    ToolInvocation inv = detect_tool_invocation(text);
    assert(inv.type == ToolInvocation::Type::SEARCH);
    assert(inv.argument == "Albert Einstein");
    assert(inv.start_pos == 24);
    assert(inv.end_pos == 49);
    std::cout << "test_detect_search_pattern: PASS" << std::endl;
}

static void test_detect_read_pattern() {
    std::string text = "I'll read that article. [READ: A/Albert_Einstein] ...";
    ToolInvocation inv = detect_tool_invocation(text);
    assert(inv.type == ToolInvocation::Type::READ);
    assert(inv.argument == "A/Albert_Einstein");
    assert(inv.start_pos == 24);
    assert(inv.end_pos == 49);
    std::cout << "test_detect_read_pattern: PASS" << std::endl;
}

static void test_no_pattern() {
    std::string text = "Just some regular text without any tool calls.";
    ToolInvocation inv = detect_tool_invocation(text);
    assert(inv.type == ToolInvocation::Type::NONE);
    assert(inv.argument.empty());
    std::cout << "test_no_pattern: PASS" << std::endl;
}

static void test_search_with_whitespace() {
    std::string text = "[SEARCH:   quantum physics  ]";
    ToolInvocation inv = detect_tool_invocation(text);
    assert(inv.type == ToolInvocation::Type::SEARCH);
    assert(inv.argument == "quantum physics");
    std::cout << "test_search_with_whitespace: PASS" << std::endl;
}

static void test_read_with_whitespace() {
    std::string text = "[READ:  A/Some_Article  ]";
    ToolInvocation inv = detect_tool_invocation(text);
    assert(inv.type == ToolInvocation::Type::READ);
    assert(inv.argument == "A/Some_Article");
    std::cout << "test_read_with_whitespace: PASS" << std::endl;
}

static void test_search_before_read() {
    // If both are present, SEARCH should be detected first (appears first in text)
    std::string text = "[SEARCH: query] and then [READ: path]";
    ToolInvocation inv = detect_tool_invocation(text);
    assert(inv.type == ToolInvocation::Type::SEARCH);
    assert(inv.argument == "query");
    std::cout << "test_search_before_read: PASS" << std::endl;
}

static void test_read_before_search() {
    // If READ appears first in the text, it should be detected first
    std::string text = "[READ: path] and then [SEARCH: query]";
    ToolInvocation inv = detect_tool_invocation(text);
    assert(inv.type == ToolInvocation::Type::READ);
    assert(inv.argument == "path");
    std::cout << "test_read_before_search: PASS" << std::endl;
}

static void test_incomplete_search() {
    std::string text = "[SEARCH: no closing bracket";
    ToolInvocation inv = detect_tool_invocation(text);
    // Should not detect since there's no closing bracket
    assert(inv.type == ToolInvocation::Type::NONE);
    std::cout << "test_incomplete_search: PASS" << std::endl;
}

static void test_empty_search() {
    std::string text = "[SEARCH:]";
    ToolInvocation inv = detect_tool_invocation(text);
    assert(inv.type == ToolInvocation::Type::SEARCH);
    assert(inv.argument.empty());
    std::cout << "test_empty_search: PASS" << std::endl;
}

static void test_multiline_content() {
    std::string text = "Let me search.\n[SEARCH: Einstein]\nHere's what I found.";
    ToolInvocation inv = detect_tool_invocation(text);
    assert(inv.type == ToolInvocation::Type::SEARCH);
    assert(inv.argument == "Einstein");
    std::cout << "test_multiline_content: PASS" << std::endl;
}

int main() {
    std::cout << "=== ZIM Tools Detection Tests ===" << std::endl;

    test_detect_search_pattern();
    test_detect_read_pattern();
    test_no_pattern();
    test_search_with_whitespace();
    test_read_with_whitespace();
    test_search_before_read();
    test_read_before_search();
    test_incomplete_search();
    test_empty_search();
    test_multiline_content();

    std::cout << "\n=== All tests passed! ===" << std::endl;
    return 0;
}
