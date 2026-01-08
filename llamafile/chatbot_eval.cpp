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

#include "chatbot.h"
#include "llama.cpp/common/base64.hpp"
#include "llama.cpp/common/common.h"
#include "llama.cpp/include/llama.h"
#include "llamafile/datauri.h"
#include "llamafile/image.h"
#include "llamafile/llama.h"
#include "llamafile/strlib.h"
#include <cassert>
#include <string>
#include <vector>

namespace lf {
namespace chatbot {

bool eval_tokens(std::vector<llama_token> tokens) {
    int N = (int)tokens.size();
    if (tokens_used() + N > llama_n_ctx(g_ctx))
        return out_of_context(N);
    for (int i = 0; i < N; i += g_params.n_batch) {
        if (g_got_sigint) {
            g_got_sigint = false;
            clear_ephemeral();
            return false;
        }
        if (N > g_params.n_batch)
            print_ephemeral(format("loading prompt %d%%...", (int)((double)i / N * 100)));
        int n_eval = (int)tokens.size() - i;
        if (n_eval > g_params.n_batch)
            n_eval = g_params.n_batch;
        if (llama_decode(g_ctx, llama_batch_get_one(&tokens[i], n_eval)))
            return out_of_context(n_eval);
        g_history.insert(g_history.end(), tokens.begin() + i, tokens.begin() + i + n_eval);
    }
    clear_ephemeral();
    // this function is what computes /stats. we need to call it now
    // since llama_decode() kicks the can down the road to functions
    // like llama_sampling_sample(). that is bad because the chatbot
    // returns control to the repl rather than sampling when loading
    // system and image prompts.
    llama_synchronize(g_ctx);
    return true;
}

// TODO: Migrate to mtmd API for multimodal support
// The old llava_image_embed API has been replaced with mtmd in the new llama.cpp
bool eval_image(const std::string_view binary) {
    (void)binary;
    err("image support requires migration to mtmd API");
    return false;
}

bool eval_token(int id) {
    return eval_tokens({id});
}

bool eval_plain_text(const std::string &str, bool add_special, bool parse_special) {
    return eval_tokens(llamafile_tokenize(g_model, str, add_special, parse_special));
}

bool eval_string(std::string_view s, bool add_special, bool parse_special) {
    size_t i = 0;
    for (;;) {
        size_t pos = s.find("data:", i);
        if (pos == std::string_view::npos)
            return eval_plain_text(std::string(s), add_special, parse_special);
        i = pos + 5;
        DataUri uri;
        size_t end = uri.parse(s.substr(pos + 5));
        if (end == std::string_view::npos)
            continue;
        if (!lf::startscasewith(uri.mime, "image/"))
            continue;
        std::string image;
        try {
            image = uri.decode();
        } catch (const base64_error &e) {
            continue;
        }
        if (!is_image(image))
            continue;
        if (!eval_plain_text(std::string(s.substr(0, pos)), add_special, parse_special))
            return false;
        if (!eval_image(image))
            return false;
        s = s.substr(i + end);
        i = 0;
    }
}

} // namespace chatbot
} // namespace lf
