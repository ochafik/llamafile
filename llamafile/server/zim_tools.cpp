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
#include "zim.h"
#include "llamafile/llamafile.h"
#include "llamafile/zim/zim.h"
#include <cstring>
#include <algorithm>

namespace lf {
namespace server {

bool
zim_tools_available()
{
    return FLAG_zim_tools && zim_is_loaded();
}

std::string
get_zim_tools_system_prompt()
{
    if (!zim_tools_available())
        return "";

    return R"(
You have access to a Wikipedia knowledge base. You can search and read articles using these tools:

1. To search for articles: [SEARCH: your search query]
   Example: [SEARCH: Albert Einstein]
   This will return a list of matching articles with their titles and snippets.

2. To read an article: [READ: article path]
   Example: [READ: A/Albert_Einstein]
   This will return the full text content of the article.

When answering questions that require factual information, use these tools to look up accurate information. First search for relevant articles, then read them to get detailed information.

Important: After using a tool, wait for the result before continuing your response. The result will be provided in a [RESULT: ...] block.
)";
}

ToolInvocation
detect_tool_invocation(const std::string& text)
{
    ToolInvocation result;

    // Find positions of both patterns
    size_t search_pos = text.find("[SEARCH:");
    size_t read_pos = text.find("[READ:");

    // Determine which pattern appears first
    bool try_search_first = true;
    if (search_pos == std::string::npos && read_pos != std::string::npos) {
        try_search_first = false;
    } else if (search_pos != std::string::npos && read_pos != std::string::npos) {
        try_search_first = (search_pos < read_pos);
    }

    if (try_search_first && search_pos != std::string::npos) {
        size_t end_pos = text.find(']', search_pos);
        if (end_pos != std::string::npos) {
            result.type = ToolInvocation::Type::SEARCH;
            result.start_pos = search_pos;
            result.end_pos = end_pos + 1;
            // Extract the query (skip "[SEARCH:" which is 8 chars)
            size_t query_start = search_pos + 8;
            std::string query = text.substr(query_start, end_pos - query_start);
            // Trim whitespace
            while (!query.empty() && std::isspace(query.front()))
                query.erase(0, 1);
            while (!query.empty() && std::isspace(query.back()))
                query.pop_back();
            result.argument = query;
            return result;
        }
    }

    if (read_pos != std::string::npos) {
        size_t end_pos = text.find(']', read_pos);
        if (end_pos != std::string::npos) {
            result.type = ToolInvocation::Type::READ;
            result.start_pos = read_pos;
            result.end_pos = end_pos + 1;
            // Extract the path (skip "[READ:" which is 6 chars)
            size_t path_start = read_pos + 6;
            std::string path = text.substr(path_start, end_pos - path_start);
            // Trim whitespace
            while (!path.empty() && std::isspace(path.front()))
                path.erase(0, 1);
            while (!path.empty() && std::isspace(path.back()))
                path.pop_back();
            result.argument = path;
            return result;
        }
    }

    // Fallback: try SEARCH if READ didn't match
    if (!try_search_first && search_pos != std::string::npos) {
        size_t end_pos = text.find(']', search_pos);
        if (end_pos != std::string::npos) {
            result.type = ToolInvocation::Type::SEARCH;
            result.start_pos = search_pos;
            result.end_pos = end_pos + 1;
            size_t query_start = search_pos + 8;
            std::string query = text.substr(query_start, end_pos - query_start);
            while (!query.empty() && std::isspace(query.front()))
                query.erase(0, 1);
            while (!query.empty() && std::isspace(query.back()))
                query.pop_back();
            result.argument = query;
            return result;
        }
    }

    return result;
}

std::optional<std::string>
execute_tool(const ToolInvocation& invocation)
{
    if (!zim_tools_available())
        return std::nullopt;

    ::zim_archive* archive = zim_get_archive();
    if (!archive)
        return std::nullopt;

    std::string result;

    if (invocation.type == ToolInvocation::Type::SEARCH) {
        // Perform search
        ::zim_search_result results[10];
        int count = ::zim_search(archive, invocation.argument.c_str(), results, 10);

        if (count == 0) {
            result = "[RESULT: No articles found for \"" + invocation.argument + "\"]\n";
        } else {
            result = "[RESULT: Found " + std::to_string(count) + " article(s):\n";
            for (int i = 0; i < count; i++) {
                result += "- ";
                if (results[i].title)
                    result += results[i].title;
                result += " (path: ";
                if (results[i].path)
                    result += results[i].path;
                result += ")\n";
            }
            result += "]\n";
            ::zim_search_free(results, count);
        }
    } else if (invocation.type == ToolInvocation::Type::READ) {
        // Read article
        ::zim_entry entry;
        if (!::zim_get_entry_by_path(archive, invocation.argument.c_str(), &entry)) {
            result = "[RESULT: Article not found: " + invocation.argument + "]\n";
        } else {
            // Follow redirects
            if (entry.is_redirect) {
                if (!::zim_resolve_redirect(archive, &entry)) {
                    result = "[RESULT: Could not resolve redirect]\n";
                    return result;
                }
            }

            // Get text content
            size_t content_size;
            char* content = ::zim_get_content_text(archive, &entry, &content_size);
            if (!content) {
                result = "[RESULT: Could not read article content]\n";
            } else {
                // Truncate if too long (to avoid overwhelming the context)
                const size_t max_len = 4000;
                std::string text(content, std::min(content_size, max_len));
                if (content_size > max_len) {
                    text += "\n... (truncated, " + std::to_string(content_size) + " bytes total)";
                }
                result = "[RESULT: Article \"";
                if (entry.title)
                    result += entry.title;
                result += "\":\n" + text + "]\n";
                ::zim_free(content);
            }
        }
    } else {
        return std::nullopt;
    }

    return result;
}

} // namespace server
} // namespace lf
