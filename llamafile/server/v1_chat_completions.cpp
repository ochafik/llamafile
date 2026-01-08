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

#include "client.h"
#include "chat.h"
#include "llama.cpp/include/llama.h"
#include "llama.cpp/common/sampling.h"
#include "llama.cpp/vendor/nlohmann/json.hpp"
#include "llamafile/json.h"
#include "llamafile/llama.h"
#include "llamafile/llamafile.h"
#include "llamafile/macros.h"
#include "llamafile/server/atom.h"
#include "llamafile/server/cleanup.h"
#include "llamafile/server/fastjson.h"
#include "llamafile/server/json-schema-to-grammar.h"
#include "llamafile/server/log.h"
#include "llamafile/server/server.h"
#include "llamafile/server/slot.h"
#include "llamafile/server/slots.h"
#include "llamafile/server/utils.h"
#include "llamafile/server/worker.h"
#include <string.h>
#include "llamafile/vector.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <sys/resource.h>
#include <vector>

using jt::Json;

namespace lf {
namespace server {

// Helper to convert JSON schema string to grammar
static std::string json_schema_string_to_grammar(const std::string& schema_str)
{
    auto schema = nlohmann::ordered_json::parse(schema_str);
    return json_schema_to_grammar(schema);
}

// Helper wrapper for llama_chat_apply_template that works with the new API
static std::string apply_chat_template(
    const llama_model* model,
    const char* tmpl,
    const std::vector<llama_chat_message>& messages,
    bool add_ass)
{
    // Get template from model if not provided
    std::string template_str;
    if (!tmpl || !*tmpl) {
        char buf[4096];
        int n = llama_model_meta_val_str(model, "tokenizer.chat_template", buf, sizeof(buf));
        if (n > 0) {
            template_str.assign(buf, n);
            tmpl = template_str.c_str();
        } else {
            // Fall back to chatml
            tmpl = "{% for message in messages %}{{'<|im_start|>' + message['role'] + '\n' + message['content'] + '<|im_end|>' + '\n'}}{% endfor %}{% if add_generation_prompt %}{{ '<|im_start|>assistant\n' }}{% endif %}";
        }
    }

    // First call to get the required size
    int len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), add_ass, nullptr, 0);
    if (len < 0) {
        return "";
    }

    std::string result(len, '\0');
    llama_chat_apply_template(tmpl, messages.data(), messages.size(), add_ass, &result[0], len);
    return result;
}

// Chat message with owned strings
struct ChatMessage {
    std::string role;
    std::string content;
};

// Convert ChatMessage vector to common_chat_msg vector for tool calling
static std::vector<common_chat_msg> to_common_chat_msgs(const std::vector<ChatMessage>& messages) {
    std::vector<common_chat_msg> result;
    result.reserve(messages.size());
    for (const auto& msg : messages) {
        common_chat_msg chat_msg;
        chat_msg.role = msg.role;
        chat_msg.content = msg.content;
        result.push_back(std::move(chat_msg));
    }
    return result;
}

// Generate a unique tool call ID
static std::string generate_tool_call_id() {
    std::string id = "call_";
    for (int i = 0; i < 8; ++i) {
        uint64_t w = _rand64();
        id += "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[w % 62];
    }
    return id;
}

struct V1ChatCompletionParams
{
    bool stream = false;
    bool stream_include_usage = false;
    long max_tokens = -1;
    long seed = _rand64();
    double top_p = 1;
    double temperature = 1;
    double presence_penalty = 0;
    double frequency_penalty = 0;
    std::string user;
    std::string model;
    std::vector<ChatMessage> messages;
    std::vector<std::vector<Atom>> stop;
    std::string grammar;
    // Tool calling support
    std::vector<common_chat_tool> tools;
    common_chat_tool_choice tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;
    bool parallel_tool_calls = false;
    // Chat format detected from template (for parsing tool calls)
    common_chat_format chat_format = COMMON_CHAT_FORMAT_CONTENT_ONLY;

    void add_stop(llama_model* model, const std::string& text)
    {
        stop.emplace_back();
        atomize(model, &stop.back(), text, DONT_PARSE_SPECIAL);
    }

    bool should_stop(const std::vector<Atom>& history)
    {
        for (const auto& suffix : stop)
            if (vector_ends_with(history, suffix))
                return true;
        return false;
    }

    // Convert to llama_chat_message format (pointers valid while this struct exists)
    std::vector<llama_chat_message> to_llama_messages() const
    {
        std::vector<llama_chat_message> result;
        result.reserve(messages.size());
        for (const auto& msg : messages) {
            result.push_back({msg.role.c_str(), msg.content.c_str()});
        }
        return result;
    }
};

struct V1ChatCompletionState
{
    std::string prompt;
    std::vector<Atom> atoms;
    std::string piece = "";
};

struct V1ChatCompletionResponse
{
    std::string content;
    Json json;
};

static void
cleanup_params(void* arg)
{
    delete (V1ChatCompletionParams*)arg;
}

static void
cleanup_state(void* arg)
{
    delete (V1ChatCompletionState*)arg;
}

static void
cleanup_response(void* arg)
{
    delete (V1ChatCompletionResponse*)arg;
}

static void
cleanup_sampler(void* arg)
{
    common_sampler_free((common_sampler*)arg);
}

static void
cleanup_slot(void* arg)
{
    Client* client = (Client*)arg;
    if (client->slot_) {
        client->worker_->server_->slots_->give(client->slot_);
        client->slot_ = nullptr;
    }
}

static bool
is_legal_role(const std::string_view& role)
{
    return role == "system" || //
           role == "user" || //
           role == "assistant";
}

static std::string
generate_id()
{
    std::string b = "chatcmpl-";
    for (int i = 0; i < 2; ++i) {
        uint64_t w = _rand64();
        for (int j = 0; j < 64 / 5; ++j) {
            b += "abcdefghijklmnopqrstuvwxyz012345"[w & 31];
            w >>= 5;
        }
    }
    return b;
}

static common_sampler*
create_sampler(const llama_model* model, const V1ChatCompletionParams* params)
{
    common_params_sampling sparams;
    sparams.temp = params->temperature;
    sparams.top_p = params->top_p;
    sparams.penalty_freq = params->frequency_penalty;
    sparams.penalty_present = params->presence_penalty;
    sparams.seed = params->seed;
    sparams.grammar = params->grammar;
    return common_sampler_init(model, sparams);
}

static std::string
make_event(const Json& json)
{
    std::string s = "data: ";
    s += json.toString();
    s += "\n\n";
    return s;
}

static int
has_images(const std::vector<Atom>& atoms)
{
    for (const Atom& atom : atoms)
        if (atom.is_image())
            return true;
    return false;
}

static int
count_bytes(const std::vector<ChatMessage>& messages)
{
    int n = 0;
    for (const ChatMessage& message : messages)
        n += message.content.size();
    return n;
}

bool
Client::get_v1_chat_completions_params(V1ChatCompletionParams* params)
{
    // must be json post request
    if (msg_.method != kHttpPost)
        return send_error(405);
    if (!HasHeader(kHttpContentType) ||
        !IsMimeType(HeaderData(kHttpContentType),
                    HeaderLength(kHttpContentType),
                    "application/json")) {
        return send_error(501, "Content Type Not Implemented");
    }
    if (!read_payload())
        return false;

    // object<model, messages, ...>
    auto [status, json] = Json::parse(std::string(payload_));
    if (status != Json::success)
        return send_error(400, Json::StatusToString(status));
    if (!json.isObject())
        return send_error(400, "JSON body must be an object");

    // fields openai documents that we don't support yet
    if (json.contains("audio"))
        return send_error(400, "OpenAI audio field not supported yet");
    if (json.contains("logprobs"))
        return send_error(400, "OpenAI logprobs field not supported yet");
    if (json.contains("functions"))
        return send_error(400, "OpenAI functions field (deprecated) not supported, use tools instead");
    if (json.contains("modalities"))
        return send_error(400, "OpenAI modalities field not supported yet");
    if (json.contains("top_logprobs"))
        return send_error(400, "OpenAI top_logprobs field not supported yet");
    if (json.contains("function_call"))
        return send_error(400, "OpenAI function_call field (deprecated) not supported, use tool_choice instead");

    // model: string
    Json& model = json["model"];
    if (!model.isString())
        return send_error(400, "JSON missing model string");
    params->model = model.getString();

    // messages: array<object<role:string, content:string>>
    if (!json["messages"].isArray())
        return send_error(400, "JSON missing messages array");
    std::vector<Json>& messages = json["messages"].getArray();
    if (messages.empty())
        return send_error(400, "JSON messages array is empty");
    for (Json& message : messages) {
        if (!message.isObject())
            return send_error(400, "messages array must hold objects");
        if (!message["role"].isString())
            return send_error(400, "message must have string role");
        if (!is_legal_role(message["role"].getString()))
            return send_error(400, "message role not system user assistant");

        // Accept string or array for content
        if (message["content"].isString()) {
            if (message["content"].getString().empty())
                return send_error(400, "message must not have empty content");
            params->messages.emplace_back(message["role"].getString(),
                                          message["content"].getString());
        } else if (message["content"].isArray()) {
            std::string combined_content;
            std::vector<Json>& content_array = message["content"].getArray();
            if (content_array.empty())
                return send_error(400, "message content array must not be empty");
            for (Json& part : content_array) {
                if (!part.isObject() || !part["type"].isString())
                    return send_error(400, "content array items must be objects with type");
                std::string type = part["type"].getString();
                if (type == "text") {
                    if (!part["text"].isString())
                        return send_error(400, "text part must have string text");
                    combined_content += part["text"].getString();
                } else if (type == "image_url") {
                    if (!part["image_url"].isObject() || !part["image_url"]["url"].isString())
                        return send_error(400, "image_url part must have url string");
                    // TODO collect image data (?)
                    // std::string image_url = part["image_url"]["url"].getString();
                } else {
                    return send_error(400, "unsupported content part type");
                }
            }
            if (combined_content.empty())
                return send_error(400, "message must not have empty content");
            params->messages.emplace_back(message["role"].getString(), combined_content);
        } else {
            return send_error(400, "message content must be string or array");
        }
    }

    // n: integer|null
    //
    // How many chat completion choices to generate for each input
    // message. Note that you will be charged based on the number of
    // generated tokens across all of the choices. Keep n as 1 to
    // minimize costs.
    Json& n = json["n"];
    if (!n.isNull()) {
        if (!n.isLong())
            return send_error(400, "n field must be integer");
        if (n.getLong() != 1)
            return send_error(400, "n field must be 1 if specified");
    }

    // stream: bool|null
    //
    // If set, partial message deltas will be sent, like in ChatGPT.
    // Tokens will be sent as data-only server-sent events as they
    // become available, with the stream terminated by a data: [DONE]
    // message.
    Json& stream = json["stream"];
    if (!stream.isNull()) {
        if (!stream.isBool())
            return send_error(400, "stream field must be boolean");
        params->stream = stream.getBool();

        // stream_options: object|null
        //
        // Options for the streaming response.
        Json& stream_options = json["stream_options"];
        if (!stream_options.isNull()) {
            if (!stream_options.isObject())
                return send_error(400, "stream_options field must be object");

            // include_usage: bool|null
            //
            // Include usage also for streaming responses. The actual usage will be reported before
            // the [DONE] message, but all chunks contain an empty usage field.
            Json& include_usage = stream_options["include_usage"];
            if (!include_usage.isNull()) {
                if (!include_usage.isBool())
                    return send_error(400, "include_usage field must be boolean");
                params->stream_include_usage = include_usage.getBool();
            }
        }
    }

    // max_tokens: integer|null
    //
    // An upper bound for the number of tokens that can be generated for
    // a completion. This can be used to control compute costs.
    Json& max_tokens = json["max_tokens"];
    if (!max_tokens.isNull()) {
        if (!max_tokens.isLong())
            return send_error(400, "max_tokens must be integer");
        params->max_tokens = max_tokens.getLong();
    }
    Json& max_completion_tokens = json["max_completion_tokens"];
    if (!max_completion_tokens.isNull()) {
        if (!max_completion_tokens.isLong())
            return send_error(400, "max_completion_tokens must be integer");
        params->max_tokens = max_completion_tokens.getNumber();
    }

    // top_p: number|null
    //
    // An alternative to sampling with temperature, called nucleus
    // sampling, where the model considers the results of the tokens
    // with top_p probability mass. So 0.1 means only the tokens
    // comprising the top 10% probability mass are considered.
    //
    // We generally recommend altering this or temperature but not both.
    Json& top_p = json["top_p"];
    if (!top_p.isNull()) {
        if (!top_p.isNumber())
            return send_error(400, "top_p must be number");
        params->top_p = top_p.getNumber();
        if (!(0 <= params->top_p && params->top_p <= 1))
            return send_error(400, "top_p must be between 0 and 1");
    }

    // temperature: number|null
    //
    // What sampling temperature to use, between 0 and 2. Higher values
    // like 0.8 will make the output more random, while lower values
    // like 0.2 will make it more focused and deterministic.
    //
    // We generally recommend altering this or top_p but not both.
    Json& temperature = json["temperature"];
    if (!temperature.isNull()) {
        if (!temperature.isNumber())
            return send_error(400, "temperature must be number");
        params->temperature = temperature.getNumber();
        if (!(0 <= params->temperature && params->temperature <= 2))
            return send_error(400, "temperature must be between 0 and 2");
    }

    // seed: integer|null
    //
    // If specified, our system will make a best effort to sample
    // deterministically, such that repeated requests with the same seed
    // and parameters should return the same result. Determinism is not
    // guaranteed, and you should refer to the system_fingerprint
    // response parameter to monitor changes in the backend.
    Json& seed = json["seed"];
    if (!seed.isNull()) {
        if (!seed.isLong())
            return send_error(400, "seed must be integer");
        params->seed = seed.getLong();
    }

    // presence_penalty: number|null
    //
    // Number between -2.0 and 2.0. Positive values penalize new tokens
    // based on whether they appear in the text so far, increasing the
    // model's likelihood to talk about new topics.
    Json& presence_penalty = json["presence_penalty"];
    if (!presence_penalty.isNull()) {
        if (!presence_penalty.isNumber())
            return send_error(400, "presence_penalty must be number");
        params->presence_penalty = presence_penalty.getNumber();
        if (!(-2 <= params->presence_penalty && params->presence_penalty <= 2))
            return send_error(400, "presence_penalty must be between -2 and 2");
    }

    // frequency_penalty: number|null
    //
    // Number between -2.0 and 2.0. Positive values penalize new tokens
    // based on their existing frequency in the text so far, decreasing
    // the model's likelihood to repeat the same line verbatim.
    Json& frequency_penalty = json["frequency_penalty"];
    if (!frequency_penalty.isNull()) {
        if (!frequency_penalty.isNumber())
            return send_error(400, "frequency_penalty must be number");
        params->frequency_penalty = frequency_penalty.getNumber();
        if (!(-2 <= params->frequency_penalty &&
              params->frequency_penalty <= 2))
            return send_error(400, "frequency_penalty must be -2 through 2");
    }

    // user: string|null
    //
    // A unique identifier representing your end-user, which can help
    // llamafiler to monitor and detect abuse.
    Json& user = json["user"];
    if (!user.isNull()) {
        if (!user.isString())
            return send_error(400, "JSON missing user string");
        params->user = user.getString();
    }

    // stop: string|array<string>|null
    //
    // Up to 4 sequences where the API will stop generating further tokens.
    Json& stop = json["stop"];
    if (!stop.isNull()) {
        if (stop.isString()) {
            params->add_stop(model_, stop.getString());
        } else if (stop.isArray()) {
            std::vector<Json>& stops = stop.getArray();
            if (stops.size() > 4)
                return send_error(400, "stop array must have 4 items or fewer");
            for (Json& stop2 : stops) {
                if (!stop2.isString())
                    return send_error(400, "stop array item must be string");
                if (stop2.getString().size() > 50)
                    return send_error(400, "stop array string too long");
                params->add_stop(model_, stop2.getString());
            }
        } else {
            return send_error(400, "stop field must be string or string array");
        }
    }

    // response_format: "auto"
    // response_format: { "type": "json_object" }
    // response_format: { "type": "json_schema", "json_schema": {...} }
    //
    // An object specifying the format that the model must output.
    //
    // Setting to { "type": "json_schema", "json_schema": {...} }
    // enables Structured Outputs which ensures the model will match
    // your supplied JSON schema. Learn more in the Structured Outputs
    // guide.
    //
    // Setting to { "type": "json_object" } enables JSON mode, which
    // ensures the message the model generates is valid JSON.
    //
    // When using JSON mode, you must also instruct the model to produce
    // JSON yourself via a system or user message. Without this, the
    // model may generate an unending stream of whitespace until the
    // generation reaches the token limit, resulting in a long-running
    // and seemingly "stuck" request. Also note that the message content
    // may be partially cut off if finish_reason = "length", which
    // indicates the generation exceeded max_tokens or the conversation
    // exceeded the max context length.
    Json& response_format = json["response_format"];
    if (!response_format.isNull()) {
        if (response_format.isString()) {
            if (response_format.getString() != "auto")
                return send_error(400, "response_format not supported");
        } else if (response_format.isObject()) {
            Json& type = response_format.getObject()["type"];
            if (!type.isString())
                return send_error(400, "response_format.type must be string");
            if (type.getString() == "json_object") {
                params->grammar =
                  json_schema_string_to_grammar("{\"type\": \"object\"}");
            } else if (type.getString() == "json_schema") {
                Json& json_schema = response_format.getObject()["json_schema"];
                if (!json_schema.isObject())
                    return send_error(
                      400, "response_format.json_schema must be object");
                try {
                    params->grammar =
                      json_schema_string_to_grammar(json_schema.toString());
                } catch (const std::exception& e) {
                    SLOG("error: couldn't compile json schema: %s", e.what());
                    return send_error(400, "bad json schema");
                }
            } else {
                return send_error(400, "response_format.type unsupported");
            }
        } else {
            return send_error(400, "response_format must be string or object");
        }
    }

    // tools: array<object>|null
    //
    // A list of tools the model may call. Currently, only functions are
    // supported as a tool. Use this to provide a list of functions the model
    // may generate JSON inputs for.
    Json& tools = json["tools"];
    if (!tools.isNull()) {
        if (!tools.isArray())
            return send_error(400, "tools must be an array");
        std::vector<Json>& tools_array = tools.getArray();
        for (Json& tool : tools_array) {
            if (!tool.isObject())
                return send_error(400, "each tool must be an object");
            Json& type = tool["type"];
            if (!type.isString() || type.getString() != "function")
                return send_error(400, "tool type must be 'function'");
            Json& function = tool["function"];
            if (!function.isObject())
                return send_error(400, "tool function must be an object");
            Json& name = function["name"];
            if (!name.isString())
                return send_error(400, "tool function name must be a string");
            common_chat_tool chat_tool;
            chat_tool.name = name.getString();
            if (function["description"].isString())
                chat_tool.description = function["description"].getString();
            if (function["parameters"].isObject())
                chat_tool.parameters = function["parameters"].toString();
            else
                chat_tool.parameters = "{}";
            params->tools.push_back(std::move(chat_tool));
        }
    }

    // tool_choice: string|object|null
    //
    // Controls which (if any) tool is called by the model.
    // "none" means the model will not call any tool.
    // "auto" means the model can pick between generating a message or calling tools.
    // "required" means the model must call one or more tools.
    // Or specify a particular tool: {"type": "function", "function": {"name": "my_function"}}
    Json& tool_choice = json["tool_choice"];
    if (!tool_choice.isNull()) {
        if (tool_choice.isString()) {
            std::string choice = tool_choice.getString();
            if (choice == "none") {
                params->tool_choice = COMMON_CHAT_TOOL_CHOICE_NONE;
            } else if (choice == "auto") {
                params->tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;
            } else if (choice == "required") {
                params->tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED;
            } else {
                return send_error(400, "tool_choice must be 'none', 'auto', 'required', or an object");
            }
        } else if (tool_choice.isObject()) {
            // Specific tool choice - treat as required for now
            // TODO: Could filter grammar to only allow this specific function
            params->tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        } else {
            return send_error(400, "tool_choice must be string or object");
        }
    }

    // parallel_tool_calls: boolean|null
    //
    // Whether to enable parallel function calling during tool use.
    Json& parallel_tool_calls = json["parallel_tool_calls"];
    if (!parallel_tool_calls.isNull()) {
        if (!parallel_tool_calls.isBool())
            return send_error(400, "parallel_tool_calls must be boolean");
        params->parallel_tool_calls = parallel_tool_calls.getBool();
    }

    return true;
}

bool
Client::v1_chat_completions()
{
    // get parameters
    auto params = new V1ChatCompletionParams;
    defer_cleanup(cleanup_params, params);
    if (!get_v1_chat_completions_params(params))
        return false;

    // create state and response objects
    V1ChatCompletionState* state = new V1ChatCompletionState;
    defer_cleanup(cleanup_state, state);
    V1ChatCompletionResponse* response = new V1ChatCompletionResponse;
    defer_cleanup(cleanup_response, response);

    // Initialize chat templates for tool support
    common_chat_templates_ptr chat_templates;
    if (!params->tools.empty()) {
        chat_templates = common_chat_templates_init(model_, FLAG_chat_template ? FLAG_chat_template : "");
    }

    // turn prompt into atom array that'll fit in context window
    for (;;) {
        // add bos token if it's needed
        const llama_vocab* vocab = llama_model_get_vocab(model_);
        if (llama_vocab_get_add_bos(vocab) && params->tools.empty())
            state->atoms.emplace_back(llama_vocab_bos(vocab));

        // turn text into tokens
        if (!params->tools.empty() && chat_templates) {
            // Use chat templates with tool support
            common_chat_templates_inputs inputs;
            inputs.messages = to_common_chat_msgs(params->messages);
            inputs.tools = params->tools;
            inputs.tool_choice = params->tool_choice;
            inputs.parallel_tool_calls = params->parallel_tool_calls;
            inputs.add_generation_prompt = true;
            inputs.use_jinja = true;
            if (!params->grammar.empty()) {
                inputs.grammar = params->grammar;
            }

            common_chat_params chat_params = common_chat_templates_apply(chat_templates.get(), inputs);
            state->prompt = chat_params.prompt;
            params->chat_format = chat_params.format;

            // Apply grammar from chat template if tools provided and no explicit grammar
            if (params->grammar.empty() && !chat_params.grammar.empty()) {
                params->grammar = chat_params.grammar;
            }

            // Add any additional stop sequences from chat template
            for (const auto& stop : chat_params.additional_stops) {
                params->add_stop(model_, stop);
            }
        } else {
            // Use simple template application (no tools)
            auto llama_msgs = params->to_llama_messages();
            state->prompt = apply_chat_template(
              model_, FLAG_chat_template, llama_msgs, ADD_ASSISTANT);
        }
        atomize(model_, &state->atoms, state->prompt, PARSE_SPECIAL);

        // we don't support multiple images yet
        state->atoms = remove_old_image_atoms(state->atoms);

        // acquire best slot
        if (!slot_) {
            slot_ = worker_->server_->slots_->take(state->atoms);
            defer_cleanup(cleanup_slot, this);
        }

        // check if image uploading is supported
        if (!slot_->clip_ctx_ && has_images(state->atoms))
            return send_error(400, "no_vision_model");

        // check if we have enough context
        int space = slot_->ctx_size();
        int avail = space - space * FLAG_reserve_tokens;
        int need = count_tokens(state->atoms);
        unassert(avail > 0);
        if (need <= avail)
            break;

        // calling atomize() again will append
        state->atoms.clear();

        // forget old messages to clear up space in context window
        //
        // to avoid calling tokenize quadratically, we use a crude byte
        // count heuristic to determine how many messages to drop. this
        // is needed since the client sends the whole history each time
        unassert(!params->messages.empty());
        int keep_msgs = params->messages[0].role == "system";
        int max_forget_msgs = (int)params->messages.size() - (keep_msgs + 1);
        if (max_forget_msgs <= 0) {
            SLOG("ran out of chat messages to forget");
            return send_error(400, "out_of_context_due_to_long_message");
        }
        int bytes = count_bytes(params->messages);
        double percent_to_remember = (double)avail / need;
        int bytes_to_delete = bytes * (1 - percent_to_remember);
        auto first = params->messages.begin();
        if (keep_msgs)
            ++first;
        auto last = first;
        int bytes_deleted = 0;
        int forgotten_msgs = 0;
        do {
            bytes_deleted += last->content.size();
            ++forgotten_msgs;
            ++last;
        } while (bytes_deleted < bytes_to_delete &&
                 forgotten_msgs < max_forget_msgs);
        SLOG("forgot %d / %zu old messages",
             forgotten_msgs,
             params->messages.size());
        params->messages.erase(first, last);
    }

    // init sampling
    common_sampler* sampler = create_sampler(model_, params);
    if (!sampler)
        return send_error(500, "failed to create sampler");
    defer_cleanup(cleanup_sampler, sampler);

    // setup response json
    response->json["id"] = generate_id();
    response->json["object"] = "chat.completion";
    response->json["model"] = params->model;
    response->json["x_prefill_progress"] = nullptr;
    response->json["system_fingerprint"] = slot_->system_fingerprint_;
    Json& choice = response->json["choices"][0];
    choice["index"] = 0;
    choice["logprobs"] = nullptr;
    choice["finish_reason"] = nullptr;

    // initialize response
    if (params->stream) {
        char* p = append_http_response_message(obuf_.p, 200);
        p = stpcpy(p, "Content-Type: text/event-stream\r\n");
        if (!send_response_start(obuf_.p, p))
            return false;
        choice["delta"]["role"] = "assistant";
        choice["delta"]["content"] = "";
        if (params->stream_include_usage)
            response->json["usage"] = nullptr;
    }

    // prefill time
    int prompt_tokens = 0;
    if (params->stream) {
        auto progress_callback = [&](int processed, int total) {
            if (processed < total) {
                response->json["x_prefill_progress"] =
                  static_cast<float>(processed) / total;
                response->json["created"] = timespec_real().tv_sec;
                response->content = make_event(response->json);
                if (!send_response_chunk(response->content)) {
                    return; // Note: Can't properly handle error in callback
                }
            }
        };
        prompt_tokens = slot_->prefill(state->atoms, progress_callback);
    } else {
        prompt_tokens = slot_->prefill(state->atoms);
    }

    if (prompt_tokens < 0) {
        SLOG("slot prefill failed: %s", Slot::describe_error(prompt_tokens));
        if (!params->stream) {
            return send_error(500, Slot::describe_error(prompt_tokens));
        } else {
            close_connection_ = true;
            return false;
        }
    }

    // initialize response
    if (params->stream) {
        response->json.getObject().erase("x_prefill_progress");
        response->content = make_event(response->json);
        choice.getObject().erase("delta");
        if (!send_response_chunk(response->content))
            return false;
    }

    // prediction time
    int completion_tokens = 0;
    const char* finish_reason = "length";
    for (;;) {
        if (params->max_tokens >= 0 &&
            completion_tokens >= params->max_tokens) {
            slot_->eval_token(llamafile_token_eot(model_));
            break;
        }
        llama_token id = common_sampler_sample(sampler, slot_->ctx_, -1);
        common_sampler_accept(sampler, id, /* accept_grammar */ true);
        ++completion_tokens;
        if (slot_->eval_token(id) < 0) {
            SLOG("ran out of context window");
            break;
        }
        if (llama_vocab_is_eog(llama_model_get_vocab(model_), id)) {
            finish_reason = "stop";
            break;
        }
        if (params->should_stop(slot_->history_)) {
            slot_->eval_token(llamafile_token_eot(model_));
            finish_reason = "stop";
            break;
        }
        state->piece +=
          llamafile_token_to_piece(slot_->ctx_, id, DONT_RENDER_SPECIAL_TOKENS);
        if (!state->piece.empty()) {
            if (params->stream) {
                if (!ends_with_incomplete_utf8(state->piece)) {
                    char* p = append_http_response_message(obuf_.p, 200);
                    choice["delta"]["content"] = state->piece;
                    response->json["created"] = timespec_real().tv_sec;
                    response->content = make_event(response->json);
                    choice.getObject().erase("delta");
                    if (!send_response_chunk(response->content))
                        return false;
                    state->piece.clear();
                }
            } else {
                response->content += state->piece;
                state->piece.clear();
            }
        }
    }
    // Parse tool calls from the generated content if tools were provided
    common_chat_msg parsed_msg;
    bool has_tool_calls = false;
    if (!params->tools.empty() && params->chat_format != COMMON_CHAT_FORMAT_CONTENT_ONLY) {
        common_chat_syntax syntax;
        syntax.format = params->chat_format;
        syntax.parse_tool_calls = true;
        try {
            parsed_msg = common_chat_parse(response->content, false, syntax);
            has_tool_calls = !parsed_msg.tool_calls.empty();
            if (has_tool_calls) {
                finish_reason = "tool_calls";
                // Assign IDs to tool calls that don't have them
                for (auto& tc : parsed_msg.tool_calls) {
                    if (tc.id.empty()) {
                        tc.id = generate_tool_call_id();
                    }
                }
            }
        } catch (const std::exception& e) {
            SLOG("warning: failed to parse tool calls: %s", e.what());
            // Continue without tool calls if parsing fails
        }
    }

    choice["finish_reason"] = finish_reason;
    SLOG("predicted %d tokens finished on %s%s", //
         completion_tokens,
         finish_reason,
         has_tool_calls ? " (with tool calls)" : "");

    // finalize response
    cleanup_slot(this);
    if (params->stream) {
        choice["delta"]["content"] = "";
        response->json["created"] = timespec_real().tv_sec;
        if (params->stream_include_usage) {
            Json& usage = response->json["usage"];
            usage["prompt_tokens"] = prompt_tokens;
            usage["completion_tokens"] = completion_tokens;
            usage["total_tokens"] = completion_tokens + prompt_tokens;
        }
        response->content = make_event(response->json);
        choice.getObject().erase("delta");
        if (!send_response_chunk(response->content))
            return false;
        if (!send_response_chunk("data: [DONE]\n\n"))
            return false;
        return send_response_finish();
    } else {
        Json& usage = response->json["usage"];
        usage["prompt_tokens"] = prompt_tokens;
        usage["completion_tokens"] = completion_tokens;
        usage["total_tokens"] = completion_tokens + prompt_tokens;
        choice["message"]["role"] = "assistant";

        if (has_tool_calls) {
            // Format tool calls in OpenAI-compatible format
            choice["message"]["content"] = parsed_msg.content.empty() ? Json() : Json(parsed_msg.content);
            Json& tool_calls = choice["message"]["tool_calls"];
            tool_calls.setArray();
            for (size_t i = 0; i < parsed_msg.tool_calls.size(); ++i) {
                const auto& tc = parsed_msg.tool_calls[i];
                Json tool_call;
                tool_call["id"] = tc.id;
                tool_call["type"] = "function";
                tool_call["function"]["name"] = tc.name;
                tool_call["function"]["arguments"] = tc.arguments;
                tool_calls.getArray().push_back(std::move(tool_call));
            }
        } else {
            choice["message"]["content"] = std::move(response->content);
        }

        response->json["created"] = timespec_real().tv_sec;
        char* p = append_http_response_message(obuf_.p, 200);
        p = stpcpy(p, "Content-Type: application/json\r\n");
        response->content = response->json.toStringPretty();
        response->content += '\n';
        return send_response(obuf_.p, p, response->content);
    }
}

} // namespace server
} // namespace lf
