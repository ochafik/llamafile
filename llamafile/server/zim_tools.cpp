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

    // System prompt tells the model about available Wikipedia tools
    // The client will detect tool patterns and execute them via /zim/* endpoints
    return R"(
You have access to a Wikipedia knowledge base. You can search and read articles using these tools:

1. To search for articles: [SEARCH: your search query]
   Example: [SEARCH: Albert Einstein]
   This will return a list of matching articles with their titles and snippets.

2. To read an article: [READ: article path]
   Example: [READ: A/Albert_Einstein]
   This will return the full text content of the article.

When answering questions that require factual information, use these tools to look up accurate information. First search for relevant articles, then read them to get detailed information.

Important: After using a tool, wait for the result before continuing your response. The result will be provided automatically.
)";
}

} // namespace server
} // namespace lf
