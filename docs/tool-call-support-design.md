# Tool Call Support for Llamafiler

## Design Document & Implementation Plan

**Author:** Claude Code
**Date:** 2025-12-27
**Status:** Draft v3 - Llamafiler Enhancement

---

## Executive Summary

This document outlines the plan to add OpenAI-compatible tool/function calling support to **llamafiler** (`llamafile/server/`), Mozilla's new high-performance server intended to replace the legacy llama.cpp server. The approach integrates llama.cpp's proven chat parsing infrastructure while preserving llamafiler's clean architecture.

---

## 1. Architecture Overview

### 1.1 Llamafile Components

```
llamafile (unified binary)
│
├── CLI Modes
│   ├── Chatbot REPL      → lf::chatbot::main()     (default)
│   ├── CLI Completion    → main.cpp with -p/-f
│   └── Embedding         → embedding_cli()
│
├── Server Modes
│   ├── Legacy Server     → server_cli()            (--server)
│   │   └── llama.cpp/server/server.cpp             ← OLD, no tools
│   │
│   └── Llamafiler        → lf::server::main()      (--v2 or "llamafiler")
│       └── llamafile/server/                        ← NEW, target for tools
│
└── Separate Binaries
    ├── sdfile            → stable-diffusion.cpp/main
    └── whisperfile       → whisper.cpp/main
```

### 1.2 Llamafiler Architecture

```
llamafile/server/
├── main.cpp              # Entry point
├── prog.cpp              # Program initialization
├── server.cpp/.h         # HTTP server core
├── client.cpp/.h         # Request handling (23KB)
├── slot.cpp/.h           # Inference slot management
├── slots.cpp/.h          # Slot pool
├── worker.cpp/.h         # Worker threads
│
├── v1_chat_completions.cpp   ← PRIMARY TARGET (28KB, 750 lines)
├── v1_completions.cpp        # Raw completions
├── v1_models.cpp             # Model listing
├── embedding.cpp             # Embeddings
├── tokenize.cpp              # Tokenization
│
├── atom.cpp/.h           # Token/image atoms
├── fastjson.cpp/.h       # Fast JSON utilities
├── log.cpp/.h            # Logging
└── www/                  # Web UI assets
```

### 1.3 Current Tool Call Status

From `v1_chat_completions.cpp` lines 209-226:
```cpp
// Explicitly rejected - these are our targets!
if (json.contains("tools"))
    return send_error(400, "OpenAI tools field not supported yet");
if (json.contains("tool_choice"))
    return send_error(400, "OpenAI tool_choice field not supported yet");
if (json.contains("parallel_tool_calls"))
    return send_error(400, "parallel_tool_calls field not supported yet");
```

Current role support (lines 125-130):
```cpp
static bool is_legal_role(const std::string_view& role) {
    return role == "system" || role == "user" || role == "assistant";
    // Missing: "tool" role for tool results
}
```

---

## 2. What Needs to Change

### 2.1 Request Parsing (`v1_chat_completions.cpp`)

| Field | Current | Target |
|-------|---------|--------|
| `tools` | Rejected | Parse into `std::vector<common_chat_tool>` |
| `tool_choice` | Rejected | Parse into `common_chat_tool_choice` |
| `parallel_tool_calls` | Rejected | Boolean flag |
| `messages[].role` | system/user/assistant | + "tool" |
| `messages[].tool_calls` | Not parsed | Parse for assistant messages |
| `messages[].tool_call_id` | Not parsed | Parse for tool messages |

### 2.2 Template Rendering

| Aspect | Current | Target |
|--------|---------|--------|
| Engine | `llama_chat_apply_template()` | `common_chat_templates_apply()` via minja |
| Tool injection | None | Inject tool definitions into prompt |
| Grammar | Manual via `response_format` | Auto-generate from tool schemas |

### 2.3 Response Generation

| Aspect | Current | Target |
|--------|---------|--------|
| Output parsing | None (raw text) | `common_chat_parse()` for tool calls |
| `finish_reason` | "stop" / "length" | + "tool_calls" |
| `message.content` | Always string | `null` when tool_calls present |
| `message.tool_calls` | Not present | Array of tool call objects |

### 2.4 Streaming

| Aspect | Current | Target |
|--------|---------|--------|
| Content deltas | ✓ Supported | Keep |
| Tool call deltas | Not supported | Add via `common_chat_msg_diff` |

---

## 3. Files to Import from llama.cpp

### 3.1 Core Infrastructure (Required)

```
From: llama.cpp (peg-migration-squashed branch)
To:   llamafile/

vendor/minja/
├── minja.hpp              # Jinja2 template engine (130KB)
└── chat-template.hpp      # Chat-specific templating (24KB)

common/
├── chat.h                 # Core types: common_chat_msg, common_chat_tool, etc.
├── chat.cpp               # Template application, format detection
├── chat-parser.h          # Parser interface
├── chat-parser.cpp        # Main parsing implementation
├── chat-parser-xml-toolcall.h   # XML format config
├── chat-parser-xml-toolcall.cpp # XML parsing
├── chat-parsers-internal.h      # Internal utilities
├── json-partial.h         # Partial JSON healing (for streaming)
├── json-partial.cpp
├── regex-partial.h        # Partial regex matching
├── regex-partial.cpp
├── peg-parser.h           # PEG grammar parser
├── peg-parser.cpp
├── chat-peg-parser.h      # PEG chat parser
└── chat-peg-parser.cpp
```

### 3.2 Model-Specific Parsers (25 files)

```
common/chat-parsers/
├── generic.cpp            # Fallback parser
├── llama-3-x.cpp          # Llama 3.x (JSON-based)
├── hermes-2-pro.cpp       # Hermes (XML-based)
├── mistral-nemo.cpp       # Mistral
├── functionary-v3-2.cpp   # Functionary
├── deepseek-r1.cpp        # DeepSeek (with reasoning)
├── deepseek-v3-1.cpp
├── minimax-m2.cpp         # MiniMax (XML)
├── glm-4-5.cpp            # GLM (XML)
├── qwen3-coder-xml.cpp    # Qwen
├── granite.cpp            # IBM Granite
├── command-r7b.cpp        # Cohere
├── nemotron-v2.cpp        # NVIDIA
├── nemotron-v3.cpp
└── ... (11 more)
```

---

## 4. Implementation Plan

### Phase 1: Foundation (Submodule & Build Setup)

**Goal:** Get llama.cpp files accessible to llamafiler.

#### Step 1.1: Switch Submodule

```bash
# Update .gitmodules
git config -f .gitmodules submodule.llama.cpp.url https://github.com/ochafik/llama.cpp.git
git config -f .gitmodules submodule.llama.cpp.branch peg-migration-squashed

# Update submodule
cd llama.cpp
git fetch origin peg-migration-squashed
git checkout peg-migration-squashed
cd ..
```

#### Step 1.2: Modify Patching Process

Update `llama.cpp.patches/apply-patches.sh` to preserve chat infrastructure:

```bash
# Instead of: rm -rf common/
# Do: Selective cleanup - keep chat files
cd common
rm -f base64.hpp common.cpp common.h console.cpp console.h \
      grammar-parser.cpp grammar-parser.h \
      json-schema-to-grammar.cpp json-schema-to-grammar.h \
      json.hpp log.h ngram-cache.cpp ngram-cache.h \
      sampling.cpp sampling.h CMakeLists.txt arg.cpp arg.h \
      speculative.cpp speculative.h train.cpp train.h
# Preserved: chat.*, chat-parser*.*, json-partial.*, peg-parser.*, chat-parsers/
cd ..

# Keep vendor/minja/ (it's not currently deleted)
```

#### Step 1.3: Update Build System

Add to `llamafile/server/BUILD.mk`:

```makefile
# Tool call support sources from llama.cpp
LLAMAFILE_SERVER_CHAT_SRCS := \
    llama.cpp/common/chat.cpp \
    llama.cpp/common/chat-parser.cpp \
    llama.cpp/common/chat-parser-xml-toolcall.cpp \
    llama.cpp/common/chat-peg-parser.cpp \
    llama.cpp/common/json-partial.cpp \
    llama.cpp/common/regex-partial.cpp \
    llama.cpp/common/peg-parser.cpp \
    $(wildcard llama.cpp/common/chat-parsers/*.cpp)

LLAMAFILE_SERVER_CHAT_OBJS := $(LLAMAFILE_SERVER_CHAT_SRCS:%.cpp=o/$(MODE)/%.o)

# Add to server library
o/$(MODE)/llamafile/server/server.a: $(LLAMAFILE_SERVER_CHAT_OBJS)

# Include paths
$(LLAMAFILE_SERVER_CHAT_OBJS): private \
    CCFLAGS += \
        -I$(srcdir)/llama.cpp/vendor/minja \
        -I$(srcdir)/llama.cpp/common
```

---

### Phase 2: Request Parsing Enhancement

**Goal:** Parse tools, tool_choice, and tool-related message fields.

#### Step 2.1: Add Data Structures

Create `llamafile/server/tool_types.h`:

```cpp
#pragma once
#include "llama.cpp/common/chat.h"  // Use llama.cpp types directly

namespace lf {
namespace server {

// Re-export types for convenience
using common_chat_tool = ::common_chat_tool;
using common_chat_tool_call = ::common_chat_tool_call;
using common_chat_tool_choice = ::common_chat_tool_choice;
using common_chat_msg = ::common_chat_msg;
using common_chat_syntax = ::common_chat_syntax;

} // namespace server
} // namespace lf
```

#### Step 2.2: Extend V1ChatCompletionParams

In `v1_chat_completions.cpp`:

```cpp
#include "llama.cpp/common/chat.h"

struct V1ChatCompletionParams
{
    // Existing fields...
    bool stream = false;
    std::string model;
    std::vector<llama_chat_msg> messages;
    // ...

    // NEW: Tool call support
    std::vector<common_chat_tool> tools;
    common_chat_tool_choice tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;
    bool parallel_tool_calls = true;

    // NEW: Extended messages with tool info
    std::vector<common_chat_msg> chat_messages;  // Replaces simple messages
};
```

#### Step 2.3: Parse Tool Fields

Replace rejection with parsing in `get_v1_chat_completions_params()`:

```cpp
// tools: array<object>
if (json.contains("tools")) {
    if (!json["tools"].isArray())
        return send_error(400, "tools must be array");
    for (Json& tool : json["tools"].getArray()) {
        if (!tool.isObject())
            return send_error(400, "tool must be object");
        if (tool["type"].getString() != "function")
            return send_error(400, "only function tools supported");

        common_chat_tool ct;
        Json& func = tool["function"];
        ct.name = func["name"].getString();
        ct.description = func["description"].getString();
        ct.parameters = func["parameters"].toString();
        params->tools.push_back(std::move(ct));
    }
}

// tool_choice: "auto" | "required" | "none" | {"type": "function", "function": {"name": "..."}}
if (json.contains("tool_choice")) {
    Json& tc = json["tool_choice"];
    if (tc.isString()) {
        std::string choice = tc.getString();
        if (choice == "auto")
            params->tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;
        else if (choice == "required")
            params->tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        else if (choice == "none")
            params->tool_choice = COMMON_CHAT_TOOL_CHOICE_NONE;
    } else if (tc.isObject()) {
        // Specific function - handled by grammar constraints
        params->tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    }
}

// parallel_tool_calls: bool
if (json.contains("parallel_tool_calls")) {
    if (!json["parallel_tool_calls"].isBool())
        return send_error(400, "parallel_tool_calls must be bool");
    params->parallel_tool_calls = json["parallel_tool_calls"].getBool();
}
```

#### Step 2.4: Parse Tool Role Messages

Extend `is_legal_role()` and message parsing:

```cpp
static bool is_legal_role(const std::string_view& role) {
    return role == "system" || role == "user" ||
           role == "assistant" || role == "tool";
}

// In message parsing loop:
common_chat_msg msg;
msg.role = message["role"].getString();
msg.content = /* existing content parsing */;

// Parse tool_calls for assistant messages
if (msg.role == "assistant" && message.contains("tool_calls")) {
    for (Json& tc : message["tool_calls"].getArray()) {
        common_chat_tool_call call;
        call.id = tc["id"].getString();
        call.name = tc["function"]["name"].getString();
        call.arguments = tc["function"]["arguments"].getString();
        msg.tool_calls.push_back(std::move(call));
    }
}

// Parse tool_call_id for tool messages
if (msg.role == "tool") {
    msg.tool_call_id = message["tool_call_id"].getString();
    msg.tool_name = message["name"].getString();
}

params->chat_messages.push_back(std::move(msg));
```

---

### Phase 3: Template Integration

**Goal:** Use minja for tool-aware template rendering.

#### Step 3.1: Initialize Chat Templates

In server initialization or lazily on first request:

```cpp
#include "llama.cpp/common/chat.h"

// In Server class or global
common_chat_templates_ptr g_chat_templates;

// Initialize when model loads
g_chat_templates = common_chat_templates_init(
    model_,
    FLAG_chat_template,  // User override
    "",  // reasoning_format
    ""   // reasoning_prefix
);
```

#### Step 3.2: Replace Template Application

In `v1_chat_completions()`:

```cpp
// OLD:
// state->prompt = llama_chat_apply_template(
//   model_, FLAG_chat_template, params->messages, ADD_ASSISTANT);

// NEW:
common_chat_templates_inputs inputs;
inputs.messages = params->chat_messages;
inputs.tools = params->tools;
inputs.tool_choice = params->tool_choice;
inputs.parallel_tool_calls = params->parallel_tool_calls;
inputs.add_generation_prompt = true;

auto chat_params = common_chat_templates_apply(g_chat_templates.get(), inputs);

state->prompt = chat_params.prompt;
state->chat_syntax = chat_params.syntax;  // Store for response parsing

// Apply grammar constraints if tools are present
if (!params->tools.empty() && !chat_params.grammar.empty()) {
    params->grammar = chat_params.grammar;
}
```

---

### Phase 4: Response Parsing

**Goal:** Detect tool calls in generated output.

#### Step 4.1: Add Parsing State

```cpp
struct V1ChatCompletionState
{
    std::string prompt;
    std::vector<Atom> atoms;
    std::string piece = "";

    // NEW: Tool call parsing
    common_chat_syntax chat_syntax;
    common_chat_msg parsed_msg;       // Accumulated parsed message
    common_chat_msg prev_parsed_msg;  // For streaming diffs
    bool parse_tool_calls = false;
};
```

#### Step 4.2: Parse Output After Generation

In the generation loop, after accumulating `response->content`:

```cpp
// After generation completes:
if (state->parse_tool_calls && !params->tools.empty()) {
    try {
        state->parsed_msg = common_chat_parse(
            response->content,
            false,  // not partial
            state->chat_syntax
        );
    } catch (const std::exception& e) {
        SLOG("tool call parsing failed: %s", e.what());
        // Fall through - treat as regular content
    }
}

// Determine finish reason
if (!state->parsed_msg.tool_calls.empty()) {
    finish_reason = "tool_calls";
} else if (/* existing conditions */) {
    finish_reason = "stop";
}
```

#### Step 4.3: Format Tool Call Response

```cpp
// In response formatting:
if (!state->parsed_msg.tool_calls.empty()) {
    // Non-streaming response
    choice["message"]["role"] = "assistant";
    choice["message"]["content"] = nullptr;  // null when tool_calls present

    Json tool_calls = Json::array();
    for (const auto& tc : state->parsed_msg.tool_calls) {
        Json call;
        call["id"] = tc.id.empty() ? generate_tool_call_id() : tc.id;
        call["type"] = "function";
        call["function"]["name"] = tc.name;
        call["function"]["arguments"] = tc.arguments;
        tool_calls.push_back(std::move(call));
    }
    choice["message"]["tool_calls"] = std::move(tool_calls);
    choice["finish_reason"] = "tool_calls";
} else {
    // Regular content response (existing code)
    choice["message"]["role"] = "assistant";
    choice["message"]["content"] = std::move(response->content);
}
```

---

### Phase 5: Streaming Tool Calls

**Goal:** Stream tool call deltas as they're generated.

#### Step 5.1: Incremental Parsing

In the streaming generation loop:

```cpp
// During streaming, periodically try to parse
if (params->stream && state->parse_tool_calls) {
    std::string accumulated = response->content + state->piece;

    try {
        auto new_msg = common_chat_parse(
            accumulated,
            true,  // partial
            state->chat_syntax
        );

        // Compute diffs
        auto diffs = common_chat_msg_diff::compute_diffs(
            state->prev_parsed_msg,
            new_msg
        );

        // Send tool call deltas
        for (const auto& diff : diffs) {
            if (!diff.tool_call_delta.name.empty() ||
                !diff.tool_call_delta.arguments.empty()) {
                // Format and send tool call chunk
                send_tool_call_delta(diff);
            }
        }

        state->prev_parsed_msg = new_msg;
    } catch (const common_chat_msg_partial_exception&) {
        // Incomplete - wait for more tokens
    }
}
```

#### Step 5.2: Tool Call Delta Format

```cpp
void send_tool_call_delta(const common_chat_msg_diff& diff) {
    Json delta;
    if (!diff.tool_call_delta.name.empty() ||
        !diff.tool_call_delta.arguments.empty()) {

        Json tc_delta;
        tc_delta["index"] = diff.tool_call_index;
        if (!diff.tool_call_delta.id.empty())
            tc_delta["id"] = diff.tool_call_delta.id;
        if (!diff.tool_call_delta.name.empty()) {
            tc_delta["type"] = "function";
            tc_delta["function"]["name"] = diff.tool_call_delta.name;
        }
        if (!diff.tool_call_delta.arguments.empty()) {
            tc_delta["function"]["arguments"] = diff.tool_call_delta.arguments;
        }
        delta["tool_calls"] = Json::array({tc_delta});
    }

    choice["delta"] = delta;
    send_response_chunk(make_event(response->json));
}
```

---

## 5. Testing Strategy

### 5.1 Unit Tests

Port from llama.cpp:
- `tests/test-chat.cpp` - Chat parsing tests
- `tests/test-chat-template.cpp` - Template tests

### 5.2 Integration Tests

Create `llamafile/server/tests/test_tool_calls.py`:

```python
import requests
import json

def test_tool_call_basic():
    response = requests.post("http://localhost:8080/v1/chat/completions", json={
        "model": "test",
        "messages": [
            {"role": "user", "content": "What's the weather in Paris?"}
        ],
        "tools": [{
            "type": "function",
            "function": {
                "name": "get_weather",
                "description": "Get weather for a location",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "location": {"type": "string"}
                    },
                    "required": ["location"]
                }
            }
        }]
    })

    data = response.json()
    assert data["choices"][0]["finish_reason"] == "tool_calls"
    assert data["choices"][0]["message"]["tool_calls"][0]["function"]["name"] == "get_weather"
```

### 5.3 Model Compatibility Matrix

| Model | Format | Status |
|-------|--------|--------|
| Llama 3.1/3.2/3.3 | JSON | Priority |
| Mistral/Mixtral | JSON | Priority |
| Hermes 2/3 Pro | XML | Priority |
| Functionary v3.x | JSON | Secondary |
| DeepSeek R1/V3 | JSON+Reasoning | Secondary |
| Qwen 3 | XML | Secondary |

---

## 6. API Reference

### 6.1 Request Format (OpenAI-Compatible)

```json
{
  "model": "llama-3.1-8b",
  "messages": [
    {"role": "system", "content": "You are a helpful assistant."},
    {"role": "user", "content": "What's the weather in Paris?"},
    {"role": "assistant", "content": null, "tool_calls": [
      {"id": "call_123", "type": "function", "function": {"name": "get_weather", "arguments": "{\"location\":\"Paris\"}"}}
    ]},
    {"role": "tool", "tool_call_id": "call_123", "name": "get_weather", "content": "{\"temp\": 22, \"unit\": \"celsius\"}"}
  ],
  "tools": [
    {
      "type": "function",
      "function": {
        "name": "get_weather",
        "description": "Get weather for a location",
        "parameters": {
          "type": "object",
          "properties": {
            "location": {"type": "string", "description": "City name"}
          },
          "required": ["location"]
        }
      }
    }
  ],
  "tool_choice": "auto"
}
```

### 6.2 Response Format

```json
{
  "id": "chatcmpl-xyz",
  "object": "chat.completion",
  "created": 1703000000,
  "model": "llama-3.1-8b",
  "choices": [{
    "index": 0,
    "message": {
      "role": "assistant",
      "content": null,
      "tool_calls": [{
        "id": "call_abc123",
        "type": "function",
        "function": {
          "name": "get_weather",
          "arguments": "{\"location\": \"Paris\"}"
        }
      }]
    },
    "finish_reason": "tool_calls"
  }],
  "usage": {
    "prompt_tokens": 50,
    "completion_tokens": 20,
    "total_tokens": 70
  }
}
```

---

## 7. File Change Summary

| File | Action | Description |
|------|--------|-------------|
| `.gitmodules` | Modify | Point to ochafik/llama.cpp peg-migration-squashed |
| `llama.cpp.patches/apply-patches.sh` | Modify | Preserve common/chat*, vendor/minja |
| `llamafile/server/BUILD.mk` | Modify | Add chat infrastructure sources |
| `llamafile/server/v1_chat_completions.cpp` | Modify | Main implementation (~200 lines added) |
| `llamafile/server/client.h` | Modify | Add chat template pointer |
| `llamafile/server/tool_types.h` | Create | Type re-exports |
| `llamafile/server/server.cpp` | Modify | Initialize chat templates |

---

## 8. Timeline Estimate

| Phase | Effort | Dependencies |
|-------|--------|--------------|
| Phase 1: Foundation | 1 day | None |
| Phase 2: Request Parsing | 1 day | Phase 1 |
| Phase 3: Template Integration | 1-2 days | Phase 1, 2 |
| Phase 4: Response Parsing | 1-2 days | Phase 3 |
| Phase 5: Streaming | 1 day | Phase 4 |
| Testing & Polish | 2-3 days | All |
| **Total** | **~8-10 days** | |

---

## 9. Future Enhancements

1. **Anthropic Messages API** - Add `/v1/messages` endpoint
2. **Parallel Execution** - Execute multiple tool calls concurrently
3. **Tool Result Validation** - Validate tool results against schemas
4. **Caching** - Cache parsed templates and grammars
5. **Metrics** - Add tool call metrics to `/metrics` endpoint

---

## Appendix A: Key Data Structures

```cpp
// From llama.cpp/common/chat.h

struct common_chat_tool {
    std::string name;
    std::string description;
    std::string parameters;  // JSON schema
};

struct common_chat_tool_call {
    std::string name;
    std::string arguments;  // JSON string
    std::string id;
};

struct common_chat_msg {
    std::string role;
    std::string content;
    std::vector<common_chat_tool_call> tool_calls;
    std::string tool_name;
    std::string tool_call_id;
};

enum common_chat_tool_choice {
    COMMON_CHAT_TOOL_CHOICE_AUTO,
    COMMON_CHAT_TOOL_CHOICE_REQUIRED,
    COMMON_CHAT_TOOL_CHOICE_NONE,
};

struct common_chat_syntax {
    common_chat_format format;
    bool parse_tool_calls;
    // ...
};
```

---

## Appendix B: Quick Start Commands

```bash
# 1. Switch submodule
cd /path/to/llamafile
git config -f .gitmodules submodule.llama.cpp.url https://github.com/ochafik/llama.cpp.git
git config -f .gitmodules submodule.llama.cpp.branch peg-migration-squashed
cd llama.cpp && git fetch origin peg-migration-squashed && git checkout peg-migration-squashed && cd ..

# 2. Verify files exist
ls llama.cpp/common/chat.h llama.cpp/vendor/minja/minja.hpp

# 3. Build
make -j

# 4. Test
./o/opt/llamafile/server/main -m model.gguf --v2

# 5. Call with tools
curl http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"test","messages":[{"role":"user","content":"weather in paris?"}],"tools":[{"type":"function","function":{"name":"get_weather","parameters":{"type":"object","properties":{"location":{"type":"string"}}}}}]}'
```
