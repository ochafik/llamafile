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

#include <cmath>
#include <cosmo.h>
#include <cstdio>
#include <string>
#include <vector>

#include "llama.cpp/common/common.h"
#include "llama.cpp/common/arg.h"
#include "llama.cpp/common/sampling.h"
#include "llama.cpp/include/llama.h"
#include "llamafile/llamafile.h"
#include "llamafile/log.h"

// Forward declare ShowCrashReports from cosmopolitan libc
extern "C" void ShowCrashReports(void);

static bool eval_tokens(struct llama_context *ctx_llama, std::vector<llama_token> tokens,
                        int n_batch, int *n_past) {
    int N = (int)tokens.size();
    for (int i = 0; i < N; i += n_batch) {
        int n_eval = (int)tokens.size() - i;
        if (n_eval > n_batch)
            n_eval = n_batch;
        // New llama_batch_get_one API: only takes tokens and n_tokens
        // pos is NULL by default, positions tracked automatically
        struct llama_batch batch = llama_batch_get_one(tokens.data() + i, n_eval);
        if (llama_decode(ctx_llama, batch))
            return false; // probably ran out of context
        *n_past += n_eval;
    }
    return true;
}

static bool eval_id(struct llama_context *ctx_llama, int id, int *n_past) {
    std::vector<llama_token> tokens;
    tokens.push_back(id);
    return eval_tokens(ctx_llama, tokens, 1, n_past);
}

static bool eval_string(struct llama_context *ctx_llama, const char *str, int n_batch, int *n_past,
                        bool add_bos) {
    std::string str2 = str;
    // New llama_tokenize API: requires vocab* instead of model*
    const struct llama_vocab * vocab = llama_model_get_vocab(llama_get_model(ctx_llama));
    int n_tokens = str2.size() + 2 * add_bos;
    std::vector<llama_token> embd_inp(n_tokens);
    n_tokens = llama_tokenize(vocab, str2.data(), str2.size(), embd_inp.data(), embd_inp.size(),
                              add_bos, false);
    embd_inp.resize(n_tokens);
    return eval_tokens(ctx_llama, embd_inp, n_batch, n_past);
}

int main(int argc, char **argv) {

    llamafile_check_cpu();
    ShowCrashReports();
    FLAG_log_disable = true;

    common_params params;
    params.n_ctx = 0;

    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_COMPLETION, nullptr))
        return 1;

    if (params.prompt.empty())
        params.prompt = "The";

    llama_backend_init();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = llamafile_gpu_layers(35);
    llama_model *model = llama_model_load_from_file(params.model.path.c_str(), model_params);
    if (model == NULL)
        return 2;

    llama_context_params ctx_params = llama_context_default_params();
    if (params.n_ctx != 0)
        ctx_params.n_ctx = params.n_ctx;
    llama_context *ctx = llama_init_from_model(model, ctx_params);
    if (ctx == NULL)
        return 3;

    printf("%s", params.prompt.c_str());
    int n_past = 0;
    // Use new llama_vocab_get_add_bos API
    const struct llama_vocab * vocab = llama_model_get_vocab(model);
    bool add_bos = llama_vocab_get_add_bos(vocab);
    eval_string(ctx, params.prompt.c_str(), params.n_batch, &n_past, add_bos);

    // Use new llama_sampler API
    struct llama_sampler * smpl = llama_sampler_init_greedy();
    for (;;) {
        llama_token id = llama_sampler_sample(smpl, ctx, 0);
        llama_sampler_accept(smpl, id);
        // Use new llama_vocab_is_eog API
        if (llama_vocab_is_eog(vocab, id))
            break;
        // Use new llama_token_to_piece API
        char buf[256];
        int n_chars = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
        printf("%.*s", n_chars, buf);
        fflush(stdout);
        if (!eval_id(ctx, id, &n_past))
            break;
    }
    printf("\n");

    llama_sampler_free(smpl);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
}
