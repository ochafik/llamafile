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
#include "llama.cpp/tools/mtmd/mtmd.h"
#include "llama.cpp/tools/mtmd/mtmd-helper.h"
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

bool eval_image_embed(const float *embed, int n_image_pos, int n_embd) {
    int N = n_image_pos;
    if (tokens_used() + N > llama_n_ctx(g_ctx))
        return out_of_context(N);
    for (int i = 0; i < N; i += g_params.n_batch) {
        if (g_got_sigint) {
            g_got_sigint = false;
            clear_ephemeral();
            return false;
        }
        if (N > g_params.n_batch)
            print_ephemeral(format("loading image %d%%...", (int)((double)i / N * 100)));
        int n_eval = N - i;
        if (n_eval > g_params.n_batch)
            n_eval = g_params.n_batch;
        llama_batch batch = {
            .n_tokens = n_eval,
            .embd = (float *)(embed + i * n_embd),
            .pos = nullptr,  // auto-tracked by llama_decode
            .n_seq_id = nullptr,
            .seq_id = nullptr,
            .logits = nullptr,
        };
        if (llama_decode(g_ctx, batch))
            return out_of_context(n_eval);
        for (int i = 0; i < n_eval; ++i)
            g_history.push_back(IMAGE_PLACEHOLDER_TOKEN);
    }
    clear_ephemeral();
    llama_synchronize(g_ctx);
    return true;
}

bool eval_image(const std::string_view binary) {
    unassert(g_mtmd);

    // Step 1: Load image from buffer
    print_ephemeral("analyzing image...");
    mtmd_bitmap* bitmap = mtmd_helper_bitmap_init_from_buf(
        g_mtmd,
        (const unsigned char*)binary.data(),
        binary.size());
    clear_ephemeral();
    if (!bitmap) {
        err("failed to load image");
        return false;
    }

    // Step 2: Tokenize with marker text
    mtmd_input_text input = {
        .text = mtmd_default_marker(),
        .add_special = false,
        .parse_special = true,
    };
    mtmd_input_chunks* chunks = mtmd_input_chunks_init();
    int32_t res = mtmd_tokenize(g_mtmd, chunks, &input, (const mtmd_bitmap**)&bitmap, 1);
    mtmd_bitmap_free(bitmap);

    if (res != 0) {
        err("failed to tokenize image");
        mtmd_input_chunks_free(chunks);
        return false;
    }

    // Step 3: Encode
    const mtmd_input_chunk* chunk = mtmd_input_chunks_get(chunks, 0);
    res = mtmd_encode_chunk(g_mtmd, chunk);
    if (res != 0) {
        err("failed to encode image");
        mtmd_input_chunks_free(chunks);
        return false;
    }

    // Step 4: Get embeddings and evaluate
    float* embd = mtmd_get_output_embd(g_mtmd);
    size_t N = mtmd_input_chunk_get_n_tokens(chunk);
    int n_embd = llama_model_n_embd(llama_get_model(g_ctx));
    bool ok = eval_image_embed(embd, N, n_embd);

    mtmd_input_chunks_free(chunks);
    return ok;
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
