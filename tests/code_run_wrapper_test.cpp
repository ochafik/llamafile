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

//
// Unit test for the code_run_js wrapping helpers (llamafile/browser_tool.h),
// the pure, httplib-free core of the Phase-3 headless-CDP code interpreter
// (ddoc 09 §4). These are the load-bearing string/JSON transforms; the live CDP
// evaluation itself is browser-gated and verified separately against Chrome.
//
// Asserts:
//   * build_code_eval_wrapper embeds arbitrary code as a JSON string LITERAL
//     (injection-safe: quotes/newlines/backslashes/`); the timeout is inlined;
//     console levels are hooked; the result shape is the documented object.
//   * format_code_result maps the by-value result object to an MCP body, keeps
//     native JSON result values, sets isError on a captured error, and truncates
//     oversized console / string results.

#include "browser_tool.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using browser::json;

static int g_fails = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

static bool contains(const std::string & hay, const std::string & needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // ---- build_code_eval_wrapper ----------------------------------------
    {
        std::string w = browser::build_code_eval_wrapper("40+2", 10000);
        CHECK(contains(w, "(async () =>"));     // async IIFE
        CHECK(contains(w, "Promise.race"));     // timeout race
        CHECK(contains(w, "await eval(__code)")); // completion-value eval
        CHECK(contains(w, "10000"));            // timeout inlined
        // The code is embedded as a JSON string literal, not bare.
        CHECK(contains(w, "= \"40+2\""));
        // console hooks present.
        CHECK(contains(w, "'log','info','warn','error','debug'"));
    }

    // Injection safety: code with quotes / newlines / backslashes / backticks
    // must round-trip as a JSON string literal (json(code).dump()), so no raw
    // copy of the dangerous characters appears as un-escaped JS.
    {
        std::string nasty = "console.log(\"a\\\"b\");\nlet x=`t`; x+'\\\\';";
        std::string w = browser::build_code_eval_wrapper(nasty, 500);
        // The exact JSON-encoded literal must appear verbatim in the wrapper.
        std::string lit = json(nasty).dump();
        CHECK(contains(w, "= " + lit + ";"));
        // timeout honored
        CHECK(contains(w, "500"));
    }

    // ---- format_code_result: success, native value preserved ------------
    {
        json val = { { "ok", true }, { "type", "number" }, { "result", 42 },
                     { "console", "" }, { "error", nullptr } };
        json r = browser::format_code_result(val, browser::CODE_OUTPUT_CAP_CHARS);
        CHECK(r["isError"] == false);
        std::string text = r["content"][0]["text"].get<std::string>();
        json parsed = json::parse(text);
        CHECK(parsed["type"] == "number");
        CHECK(parsed["result"] == 42);         // native int preserved
        CHECK(parsed["truncated"] == false);
        // empty console omitted
        CHECK(!parsed.contains("console"));
    }

    // ---- format_code_result: console captured ---------------------------
    {
        json val = { { "ok", true }, { "type", "number" }, { "result", 6 },
                     { "console", "hi" }, { "error", nullptr } };
        json r = browser::format_code_result(val, browser::CODE_OUTPUT_CAP_CHARS);
        CHECK(r["isError"] == false);
        json parsed = json::parse(r["content"][0]["text"].get<std::string>());
        CHECK(parsed["result"] == 6);
        CHECK(parsed["console"] == "hi");
    }

    // ---- format_code_result: error -> isError ---------------------------
    {
        json val = { { "ok", false }, { "type", "undefined" },
                     { "result", nullptr }, { "console", "" },
                     { "error", "Error: boom\n    at <anonymous>" } };
        json r = browser::format_code_result(val, browser::CODE_OUTPUT_CAP_CHARS);
        CHECK(r["isError"] == true);
        json parsed = json::parse(r["content"][0]["text"].get<std::string>());
        CHECK(contains(parsed["error"].get<std::string>(), "boom"));
    }

    // ---- format_code_result: truncation ---------------------------------
    {
        std::string big(50, 'x');
        json val = { { "ok", true }, { "type", "string" }, { "result", big },
                     { "console", big }, { "error", nullptr } };
        json r = browser::format_code_result(val, 10);
        json parsed = json::parse(r["content"][0]["text"].get<std::string>());
        CHECK(parsed["truncated"] == true);
        CHECK(parsed["result"].get<std::string>().size() == 10u);
        CHECK(parsed["console"].get<std::string>().size() == 10u);
    }

    if (g_fails == 0) {
        printf("code_run_wrapper_test: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "code_run_wrapper_test: %d check(s) failed\n", g_fails);
    return 1;
}
