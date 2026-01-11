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

#include <cosmo.h>
#include <cstdio>
#include <string>
#include <vector>

#include "llama.cpp/common/arg.h"
#include "llama.cpp/common/common.h"
#include "llama.cpp/common/sampling.h"
#include "llama.cpp/include/llama.h"
#include "llamafile/llamafile.h"

int main(int argc, char **argv) {

    llamafile_check_cpu();
    ShowCrashReports();

    common_params params;
    params.n_ctx = 0;

    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_MAIN))
        return 1;

    if (params.prompt.empty())
        params.prompt = "The";

    common_init();
    llama_backend_init();
    llama_numa_init(params.numa);

    ggml_backend_load_all();

    // Enable llamafile GPU detection (FLAGS_READY must be true before llamafile_gpu_layers)
    FLAGS_READY = true;

    // Update n_gpu_layers for llamafile
    params.n_gpu_layers = llamafile_gpu_layers(35);

    // Use common_init_from_params to initialize model and context
    common_init_result init_result = common_init_from_params(params);

    llama_model *model = init_result.model.get();
    llama_context *ctx = init_result.context.get();
    common_sampler *smpl = common_sampler_init(model, params.sampling);

    if (model == NULL)
        return 2;
    if (ctx == NULL)
        return 3;

    const struct llama_vocab *vocab = llama_model_get_vocab(model);

    printf("%s", params.prompt.c_str());

    // Tokenize the prompt
    const int n_prompt = -llama_tokenize(vocab, params.prompt.c_str(), params.prompt.size(), NULL, 0, true, true);
    std::vector<llama_token> prompt_tokens(n_prompt);
    if (llama_tokenize(vocab, params.prompt.c_str(), params.prompt.size(), prompt_tokens.data(), prompt_tokens.size(), true, true) < 0)
        return 4;

    // Evaluate prompt tokens
    // llama_batch_get_one returns a batch with pos=nullptr, which is fine
    // llama_decode handles sequential position tracking internally
    for (int i = 0; i < (int)prompt_tokens.size(); i += params.n_batch) {
        int n_eval = (int)prompt_tokens.size() - i;
        if (n_eval > params.n_batch)
            n_eval = params.n_batch;
        struct llama_batch batch = llama_batch_get_one(&prompt_tokens[i], n_eval);
        if (llama_decode(ctx, batch))
            break;
    }

    // Main generation loop
    for (;;) {
        llama_token id = common_sampler_sample(smpl, ctx, 0);
        if (llama_vocab_is_eog(vocab, id))
            break;

        common_sampler_accept(smpl, id, true);

        char buf[128];
        int n = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
        if (n < 0)
            break;
        std::string s(buf, n);
        printf("%s", s.c_str());
        fflush(stdout);

        // Evaluate the new token
        struct llama_batch batch = llama_batch_get_one(&id, 1);
        if (llama_decode(ctx, batch))
            break;
    }
    printf("\n");

    // Cleanup is handled automatically by common_init_result destructor

    return 0;
}
