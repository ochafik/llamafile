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
#include <iostream>

using namespace lf::server;

// Tool detection is now handled client-side in JavaScript.
// This test file is kept as a placeholder for potential future server-side tests.

static void test_zim_tools_available() {
    // When no ZIM file is loaded, tools should not be available
    bool available = zim_tools_available();
    // We can't assert this since it depends on runtime state
    std::cout << "zim_tools_available: " << (available ? "true" : "false") << std::endl;
}

static void test_get_system_prompt() {
    // Get the system prompt - will be empty if ZIM not available
    std::string prompt = get_zim_tools_system_prompt();
    std::cout << "System prompt length: " << prompt.size() << " bytes" << std::endl;
    if (!prompt.empty()) {
        std::cout << "System prompt starts with: " << prompt.substr(0, 50) << "..." << std::endl;
    }
}

int main() {
    std::cout << "=== ZIM Tools Tests ===" << std::endl;

    test_zim_tools_available();
    test_get_system_prompt();

    std::cout << "\n=== Tests completed ===" << std::endl;
    return 0;
}
