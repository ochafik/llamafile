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

#include <cctype>
#include <csignal>
#include <cstdio>
#include <string_view>

#include "llama.cpp/common/common.h"
#include "llama.cpp/common/sampling.h"
#include "llama.cpp/include/llama.h"
#include "llamafile/bestline.h"
#include "llamafile/color.h"
#include "llamafile/highlight/highlight.h"
#include "llamafile/llama.h"

namespace {
// Helper to apply chat template
std::string apply_chat_template(const std::string &tmpl,
                                 const std::vector<llama_chat_message> &chat,
                                 bool add_assistant) {
    const char *tmpl_cstr = tmpl.empty() ? nullptr : tmpl.c_str();
    int32_t size = llama_chat_apply_template(tmpl_cstr, chat.data(), chat.size(), add_assistant, nullptr, 0);
    if (size < 0) return "";
    std::string result(size, '\0');
    llama_chat_apply_template(tmpl_cstr, chat.data(), chat.size(), add_assistant, result.data(), result.size());
    return result;
}
} // namespace

namespace lf {
namespace chatbot {

bool g_has_ephemeral;
bool g_said_something;
char g_last_printed_char;
volatile sig_atomic_t g_got_sigint;

void on_sigint(int sig) {
    g_got_sigint = 1;
}

bool is_empty(const char *s) {
    int c;
    while ((c = *s++))
        if (!isspace(c))
            return false;
    return true;
}

void print(const std::string_view &s) {
    for (char c : s) {
        g_last_printed_char = c;
        fputc(c, stdout);
        if (c == '\n')
            g_has_ephemeral = false;
    }
}

void ensure_newline() {
    if (g_last_printed_char != '\n')
        print("\n");
}

void err(const char *fmt, ...) {
    va_list ap;
    clear_ephemeral();
    ensure_newline();
    va_start(ap, fmt);
    fputs(BRIGHT_RED, stderr);
    vfprintf(stderr, fmt, ap);
    fputs(RESET "\n", stderr);
    va_end(ap);
}

void print_ephemeral(const std::string_view &description) {
    fprintf(stderr, " " BRIGHT_BLACK "%.*s" UNFOREGROUND "\r", (int)description.size(),
            description.data());
    g_has_ephemeral = true;
}

void clear_ephemeral(void) {
    if (g_has_ephemeral) {
        fprintf(stderr, CLEAR_FORWARD);
        g_has_ephemeral = false;
    }
}

bool out_of_context(int extra) {
    err("error: ran out of context window at %d tokens\n"
        "consider passing `-c %d` at startup for the maximum\n"
        "you can free up more space using /forget or /clear",
        tokens_used() + extra, llama_model_n_ctx_train(g_model));
    return false;
}

void repl() {
    const llama_vocab *vocab = llama_model_get_vocab(g_model);

    // setup conversation
    if (llama_vocab_get_add_bos(vocab)) {
        print_ephemeral("loading bos token...");
        eval_token(llama_token_bos(vocab));
    }
    record_undo();

    // make base models have no system prompt by default
    if (is_base_model() && g_params.prompt == DEFAULT_SYSTEM_PROMPT)
        g_params.prompt = "";

    // setup system prompt
    if (!g_params.prompt.empty()) {
        print_ephemeral("loading system prompt...");
        std::string msg;
        if (is_base_model()) {
            msg = g_params.prompt;
        } else {
            llama_chat_message sysmsg = {"system", g_params.prompt.c_str()};
            std::vector<llama_chat_message> chat = {sysmsg};
            msg = apply_chat_template(g_params.chat_template, chat, DONT_ADD_ASSISTANT);
        }
        if (!eval_string(msg, DONT_ADD_SPECIAL, PARSE_SPECIAL))
            exit(6);
        llama_synchronize(g_ctx);
        g_system_prompt_tokens = tokens_used();
        clear_ephemeral();
        if (g_params.display_prompt)
            printf("%s\n", g_params.special ? msg.c_str() : g_params.prompt.c_str());
    }

    // perform important setup
    HighlightTxt txt;
    HighlightMarkdown markdown;
    ColorBleeder bleeder(is_base_model() ? (Highlight *)&txt : (Highlight *)&markdown);
    common_sampler *sampler = common_sampler_init(g_model, g_params.sampling);
    signal(SIGINT, on_sigint);

    // run chatbot
    for (;;) {
        record_undo();
        bestlineLlamaMode(true);
        bestlineSetHintsCallback(on_hint);
        bestlineSetFreeHintsCallback(free);
        bestlineSetCompletionCallback(on_completion);
        write(1, get_role_color(g_role), strlen(get_role_color(g_role)));
        char *line = bestlineWithHistory(">>> ", "llamafile");
        write(1, RESET, strlen(RESET));
        g_last_printed_char = '\n';
        if (!line) {
            if (g_got_sigint)
                ensure_newline();
            break;
        }
        if (!is_base_model() && is_empty(line)) {
            if (g_manual_mode) {
                g_role = cycle_role(g_role);
                write(1, "\033[F", 3);
            }
            free(line);
            continue;
        }
        g_said_something = true;
        if (handle_command(line)) {
            free(line);
            continue;
        }
        bool add_assi = !g_manual_mode;
        int tokens_used_before = tokens_used();
        std::string msg;
        if (is_base_model()) {
            msg = line;
        } else {
            llama_chat_message usermsg = {get_role_name(g_role), line};
            std::vector<llama_chat_message> chat = {usermsg};
            msg = apply_chat_template(g_params.chat_template, chat, add_assi);
        }
        if (!eval_string(msg, DONT_ADD_SPECIAL, PARSE_SPECIAL)) {
            rewind(tokens_used_before);
            continue;
        }
        if (g_manual_mode) {
            g_role = get_next_role(g_role);
            free(line);
            continue;
        }
        for (;;) {
            if (g_got_sigint) {
                eval_token(llamafile_token_eot(g_model));
                break;
            }
            llama_token id = common_sampler_sample(sampler, g_ctx, -1);
            common_sampler_accept(sampler, id, APPLY_GRAMMAR);
            if (!eval_token(id))
                break;
            if (llama_token_is_eog(vocab, id))
                break;
            std::string s;
            bleeder.feed(&s, token_to_piece(g_ctx, id, g_params.special));
            print(s);
            fflush(stdout);
        }
        g_got_sigint = 0;
        free(line);
        std::string s;
        bleeder.flush(&s);
        print(s);
        ensure_newline();
    }

    // cleanup resources
    common_sampler_free(sampler);
}

} // namespace chatbot
} // namespace lf
