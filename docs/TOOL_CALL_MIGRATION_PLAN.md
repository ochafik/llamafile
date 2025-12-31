# Tool Call Support Migration Plan for llamafile

**Document Version:** 1.0
**Date:** 2025-12-26
**Status:** Design Phase

## Executive Summary

This document provides a comprehensive design and migration plan for adding OpenAI-compatible tool call (function calling) support to llamafile, by porting the relevant components from llama.cpp's server implementation.

### Current Status
- **llamafile server:** NO tool call support - explicitly rejects `tools`, `function_call`, `tool_choice`, `parallel_tool_calls` fields with HTTP 400
- **llama.cpp server:** FULL tool call support with 25+ model-specific formats, JSON schema to grammar conversion, and streaming support

### Goal
Add full OpenAI-compatible tool call support to llamafile by integrating the proven tool call implementation from llama.cpp.

---

## Part 1: Architecture Comparison

### 1.1 llamafile Server Architecture

```
llamafile/llamafile/server/
├── main.cpp              - Entry point
├── prog.cpp              - Main program logic
├── server.h/cpp          - Server class (socket management, worker threads)
├── worker.h/cpp          - Worker class (individual client connections)
├── client.h/cpp          - HTTP request/response handling, API implementation
├── slots.h/cpp           - Slot management (concurrent requests, model instances)
├── atom.h/cpp            - Token processing (text and images)
├── listen.cpp            - TCP socket management
├── v1_chat_completions.cpp  - Chat completion endpoint (NO tool support)
└── fastjson.h/cpp        - Custom JSON parser
```

**Key Characteristics:**
- Custom HTTP implementation (no libcurl/httplib)
- Custom JSON implementation (`fastjson.h/cpp`) for efficiency
- Multi-threaded with configurable worker pool
- Slot-based request queuing system
- Direct use of llama.cpp core (`llama.cpp/llama.h/cpp` as submodule)

### 1.2 llama.cpp Server Architecture (Tool Call Support)

```
llama.cpp/
├── common/
│   ├── chat.h/cpp                    - Core chat functionality, tool call serialization
│   ├── chat-parser.h/cpp             - Parser for 25+ tool call formats
│   ├── chat-parser-xml-toolcall.h    - XML-style tool call parsing
│   ├── chat-peg-parser.h/cpp         - PEG parser infrastructure
│   ├── json-schema-to-grammar.h/cpp  - JSON schema -> GBNF grammar conversion
│   └── nlohmann/json.hpp             - JSON library (via nlohmann)
│
├── vendor/minja/
│   └── minja.hpp                     - Jinja2-compatible templating engine
│
├── tools/server/
│   ├── server.cpp                    - Main server entry point
│   ├── server-context.cpp/h          - Core server logic
│   ├── server-task.cpp/h             - Task processing with tool calls
│   ├── server-http.cpp/h             - HTTP handling (cpp-httplib)
│   └── server-common.cpp/h           - OAI compatibility layer
│
└── scripts/                          - Various tool call format examples
```

**Key Components for Tool Calls:**

| Component | Purpose | Dependencies |
|-----------|---------|--------------|
| `common/chat.cpp` | Tool call data structures, OpenAI JSON formatting, diff computation | nlohmann/json, minja |
| `common/chat-parser.cpp` | 25+ model-specific parsers (Llama 3.1, DeepSeek, Mistral, etc.) | regex, chat.h |
| `common/json-schema-to-grammar.cpp` | Convert tool parameter schemas to GBNF grammars | nlohmann/json |
| `vendor/minja/minja.hpp` | Chat template engine with tool call support | nlohmann/json |

### 1.3 Key Architectural Differences

| Aspect | llamafile | llama.cpp server |
|--------|-----------|------------------|
| HTTP Library | Custom implementation | cpp-httplib |
| JSON Library | Custom (`fastjson.h/cpp`) | nlohmann/json (ordered_json) |
| Chat Templates | llama.cpp C API (`llama_chat_apply_template`) | minja (Jinja2-compatible) |
| Message Format | `llama_chat_msg` (C struct) | `common_chat_msg` (C++ struct) |
| Tool Call Support | None | Full (25+ formats) |
| Streaming | Custom SSE implementation | cpp-httplib + custom |

---

## Part 2: Tool Call Implementation Deep Dive

### 2.1 Core Data Structures

**llama.cpp tool call structures:**

```cpp
// common/chat.h
struct common_chat_tool_call {
    std::string name;        // Function name
    std::string arguments;   // JSON string of arguments
    std::string id;          // Unique ID (e.g., "call_abc123")
};

struct common_chat_msg {
    std::string role;
    std::string content;
    std::string reasoning_content;      // For reasoning models (DeepSeek R1, etc.)
    std::vector<common_chat_tool_call> tool_calls;
    std::string tool_name;              // For "tool" role messages
    std::string tool_call_id;           // For "tool" role messages
    std::vector<common_chat_msg_content_part> content_parts;  // Multi-modal content
};

struct common_chat_tool {
    std::string name;
    std::string description;
    std::string parameters;  // JSON schema string
};

enum common_chat_tool_choice {
    COMMON_CHAT_TOOL_CHOICE_AUTO,      // Model decides
    COMMON_CHAT_TOOL_CHOICE_REQUIRED,  // Must use tools
    COMMON_CHAT_TOOL_CHOICE_NONE,      // No tools
};
```

### 2.2 Tool Call Request/Response Flow

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        Tool Call Request Flow                          │
└─────────────────────────────────────────────────────────────────────────┘

1. Client Request (OpenAI format)
   │
   ├─ messages: [{role: "user", content: "..."}]
   ├─ tools: [{type: "function", function: {name, description, parameters}}]
   └─ tool_choice: "auto" | "required" | "none" | {type: "function", name: "..."}
                │
                ▼
2. Parse Request (server-common.cpp)
   │
   ├─ common_chat_msgs_parse_oaicompat(messages)
   ├─ common_chat_tools_parse_oaicompat(tools)
   └─ common_chat_tool_choice_parse_oaicompat(tool_choice)
                │
                ▼
3. Apply Chat Template with Tools (chat.cpp)
   │
   ├─ common_chat_templates_apply()
   │   ├─ Use minja to render template with tools
   │   └─ Include tool definitions in prompt
   │
   └─ Convert JSON schemas to grammars (json-schema-to-grammar.cpp)
       ├─ Each tool's parameters schema -> GBNF grammar
       └─ Grammar constrains model output to valid tool calls
                │
                ▼
4. Generate Response (llama_sampling)
   │
   ├─ Model generates text following grammar constraints
   ├─ Output follows format (e.g., Llama 3.1: {"name": "func", "parameters": {...}})
   └─ Streaming deltas accumulated
                │
                ▼
5. Parse Response (chat-parser.cpp)
   │
   ├─ common_chat_parse() with format-specific parser
   │   ├─ 25+ formats: LLAMA_3_X, DEEPSEEK_R1, MISTRAL_NEMO, etc.
   │   └─ Extract tool calls from model output
   │
   └─ Handle partial responses during streaming
       ├─ Parse incomplete JSON for delta updates
       └─ Compute diffs for incremental updates
                │
                ▼
6. Format Response (chat.cpp)
   │
   ├─ common_chat_msg::to_json_oaicompat()
   ├─ Format as OpenAI-compatible response
   │
   └─ Streaming: common_chat_msg_diff::compute_diffs()
       ├─ Delta updates for tool_calls array
       └─ SSE events for each chunk
                │
                ▼
7. Client Response
   │
   └─ {
       choices: [{
         message: {
           role: "assistant",
           tool_calls: [{
             id: "call_abc123",
             type: "function",
             function: {name: "get_weather", arguments: "{\"location\": \"Paris\"}"}
           }]
         },
         finish_reason: "tool_calls"
       }]
     }
```

### 2.3 Supported Tool Call Formats (llama.cpp)

| Format | Example Output | Models |
|--------|---------------|--------|
| `LLAMA_3_X` | `{"name": "func", "parameters": {...}}` | Llama 3.1, 3.2 |
| `DEEPSEEK_R1` | `<｜tool▁calls▁begin｜>...<｜tool▁calls▁end｜>` | DeepSeek R1 |
| `DEEPSEEK_V3_1` | `<｜tool▁call▁begin｜>NAME<｜tool▁sep｜>JSON` | DeepSeek V3.1 |
| `MISTRAL_NEMO` | `[TOOL_CALLS] [{...}]` | Mistral NeMo |
| `HERMES_2_PRO` | `<tool_name>...</tool_name>` | Hermes 2 Pro |
| `GRANITE` | `<|tool_call|> [...]` | IBM Granite |
| `COMMAND_R7B` | `<|START_ACTION|>...<|END_ACTION|>` | Command R |
| `GLM_4_5` | `<tool_call>...</tool_call>` | GLM-4 |
| And 15+ more formats | | |

---

## Part 3: Design Options

### Option A: Copy-Paste Approach (Recommended)

**Approach:** Copy the tool call components from llama.cpp into llamafile's codebase.

**Pros:**
- No coupling to llama.cpp's development cycle
- Can modify and optimize for llamafile's architecture
- Clear separation of concerns
- llamafile's custom HTTP/JSON implementations remain unchanged

**Cons:**
- Code duplication
- Need to maintain synced copies
- Larger binary size

**Files to Copy:**
```
From llama.cpp -> To llamafile/llamafile/server/

common/chat.h                    -> llamafile/server/chat.h
common/chat.cpp                  -> llamafile/server/chat.cpp
common/chat-parser.h            -> llamafile/server/chat-parser.h
common/chat-parser.cpp          -> llamafile/server/chat-parser.cpp
common/chat-parser-xml-toolcall.h -> llamafile/server/chat-parser-xml-toolcall.h
common/chat-peg-parser.h        -> llamafile/server/chat-peg-parser.h
common/chat-peg-parser.cpp      -> llamafile/server/chat-peg-parser.cpp
common/json-schema-to-grammar.h -> llamafile/server/json-schema-to-grammar.h
common/json-schema-to-grammar.cpp -> llamafile/server/json-schema-to-grammar.cpp
vendor/minja/minja.hpp          -> llamafile/server/minja.hpp
vendor/minja/chat-template.hpp  -> llamafile/server/chat-template.hpp
```

### Option B: Shared Header Approach (Alternative)

**Approach:** Share headers from llama.cpp submodule, link against common object files.

**Pros:**
- No code duplication
- Automatic updates from llama.cpp

**Cons:**
- Tight coupling to llama.cpp build system
- Potential ABI compatibility issues
- Build system complexity

**Recommendation:** **Option A** is preferred for cleaner separation and easier maintenance.

---

## Part 4: Migration Plan - Phased Implementation

### Phase 1: Foundation (Week 1-2)

#### Goal: Set up core infrastructure without changing chat completions behavior

**Step 1.1: Copy core dependencies**
- [ ] Copy `vendor/minja/minja.hpp` to `llamafile/server/minja.hpp`
- [ ] Copy `vendor/minja/chat-template.hpp` to `llamafile/server/chat-template.hpp`
- [ ] Verify minja compiles standalone
- [ ] Create test program to verify minja template rendering

**Step 1.2: Copy JSON schema to grammar converter**
- [ ] Copy `common/json-schema-to-grammar.h` to `llamafile/server/json-schema-to-grammar.h`
- [ ] Copy `common/json-schema-to-grammar.cpp` to `llamafile/server/json-schema-to-grammar.cpp`
- [ ] Add `nlohmann/json` dependency to build (or adapt to fastjson)
- [ ] Create unit tests for schema -> grammar conversion

**Step 1.3: Copy chat core structures**
- [ ] Copy `common/chat.h` to `llamafile/server/chat.h`
- [ ] Copy `common/chat.cpp` to `llamafile/server/chat.cpp`
- [ ] Create adapter to bridge `llama_chat_msg` <-> `common_chat_msg`
- [ ] Verify compilation with existing codebase

**Step 1.4: Update build system**
- [ ] Add new files to `llamafile/server/BUILD.mk`
- [ ] Add nlohmann/json include path (or use existing)
- [ ] Ensure proper linkage order
- [ ] Run full build and fix any compilation errors

**Deliverable:** All tool call infrastructure code compiles, no functional changes yet

---

### Phase 2: Parser Integration (Week 2-3)

#### Goal: Integrate tool call output parsers

**Step 2.1: Copy parser infrastructure**
- [ ] Copy `common/chat-parser.h` to `llamafile/server/chat-parser.h`
- [ ] Copy `common/chat-parser.cpp` to `llamafile/server/chat-parser.cpp`
- [ ] Copy `common/chat-parser-xml-toolcall.h` to `llamafile/server/chat-parser-xml-toolcall.h`
- [ ] Verify compilation

**Step 2.2: Copy PEG parser (if needed)**
- [ ] Copy `common/chat-peg-parser.h` to `llamafile/server/chat-peg-parser.h`
- [ ] Copy `common/chat-peg-parser.cpp` to `llamafile/server/chat-peg-parser.cpp`
- [ ] Copy `common/peg-parser.h` to `llamafile/server/peg-parser.h`
- [ ] Copy `common/peg-parser.cpp` to `llamafile/server/peg-parser.cpp`

**Step 2.3: Add regex dependencies**
- [ ] Ensure `<regex>` is available (standard C++11)
- [ ] Copy `common/regex-partial.h` if needed for partial matching
- [ ] Test regex patterns for key formats (Llama 3.1, DeepSeek R1)

**Step 2.4: Create parser registry**
- [ ] Map GGUF model metadata to correct parser format
- [ ] Add fallback to generic format for unknown models
- [ ] Create unit tests for each format

**Deliverable:** All 25+ parser formats integrated and tested

---

### Phase 3: Chat Completions Integration (Week 3-4)

#### Goal: Add tool call support to /v1/chat/completions endpoint

**Step 3.1: Extend request parameters**
```cpp
// In v1_chat_completions.cpp, modify V1ChatCompletionParams:
struct V1ChatCompletionParams {
    // ... existing fields ...
    std::vector<common_chat_tool> tools;        // NEW
    common_chat_tool_choice tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;  // NEW
    bool parallel_tool_calls = false;           // NEW
};
```

**Step 3.2: Parse tool request fields**
- [ ] Remove the "not supported" errors for `tools`, `tool_choice`, etc.
- [ ] Parse `tools` array using `common_chat_tools_parse_oaicompat`
- [ ] Parse `tool_choice` using `common_chat_tool_choice_parse_oaicompat`
- [ ] Validate tool schemas

**Step 3.3: Apply chat template with tools**
- [ ] Initialize `common_chat_templates` from model metadata
- [ ] Call `common_chat_templates_apply()` with tools
- [ ] Convert tool parameter schemas to grammars
- [ ] Apply grammar constraints to sampling

**Step 3.4: Parse model output for tool calls**
- [ ] Accumulate generated text during streaming
- [ ] Call `common_chat_parse()` with appropriate format
- [ ] Extract tool calls from parsed output
- [ ] Handle partial responses for streaming

**Step 3.5: Format response with tool calls**
- [ ] Use `common_chat_msg::to_json_oaicompat()` for formatting
- [ ] Add `tool_calls` field to response
- [ ] Set `finish_reason: "tool_calls"` when appropriate
- [ ] Handle streaming with `common_chat_msg_diff::compute_diffs()`

**Step 3.6: Add "tool" role support**
- [ ] Support messages with `role: "tool"` in request
- [ ] Map to `tool_name` and `tool_call_id` fields
- [ ] Include tool results in prompt template

**Deliverable:** Full tool call support in /v1/chat/completions

---

### Phase 4: Testing & Validation (Week 4-5)

**Step 4.1: Unit tests**
- [ ] JSON schema to grammar conversion
- [ ] Tool call parsing for all formats
- [ ] OpenAI request/response serialization
- [ ] Streaming delta computation

**Step 4.2: Integration tests**
- [ ] Test with real models (Llama 3.1, DeepSeek R1, Mistral NeMo)
- [ ] Verify tool call generation
- [ ] Verify streaming tool calls
- [ ] Test parallel tool calls
- [ ] Test `tool_choice` modes

**Step 4.3: OpenAI compatibility tests**
- [ ] Test against OpenAI Python SDK
- [ ] Test against LangChain tool calling
- [ ] Test against common tool-calling frameworks

**Step 4.4: Performance tests**
- [ ] Measure latency impact
- [ ] Measure memory overhead
- [ ] Compare with llama.cpp server

**Deliverable:** Fully tested and validated tool call support

---

### Phase 5: Documentation & Polish (Week 5)

**Step 5.1: Documentation**
- [ ] Update API documentation
- [ ] Add tool call examples
- [ ] Document supported formats
- [ ] Add troubleshooting guide

**Step 5.2: Flags & Configuration**
- [ ] Add flags for tool-related settings
- [ ] Add format override flag
- [ ] Add parser selection flag

**Deliverable:** Production-ready tool call support

---

## Part 5: Integration Points

### 5.1 JSON Library Decision

**Current:** llamafile uses `fastjson.h/cpp` (custom implementation)
**llama.cpp uses:** `nlohmann/json.hpp` (header-only library)

**Options:**
1. **Keep fastjson, adapt chat.cpp** - More work, smaller binary
2. **Add nlohmann/json alongside** - Less work, larger binary

**Recommendation:** Option 2 - Add nlohmann/json as vendored header. It's header-only and well-tested.

```makefile
# In llamafile/BUILD.mk or llamafile/server/BUILD.mk
LLAMAFILE_SERVER_JSON_HEADERS := \
    third_party/nlohmann/json.hpp
```

### 5.2 Chat Template Integration

**Current:** llamafile uses `llama_chat_apply_template()` from llama.cpp C API

**New:** Use `common_chat_templates_apply()` with minja for tool support

**Migration approach:**
```cpp
// Old (no tools):
state->prompt = llama_chat_apply_template(
    model_, FLAG_chat_template, params->messages, ADD_ASSISTANT);

// New (with tools):
common_chat_templates_inputs inputs;
inputs.messages = convert_to_common_chat_msgs(params->messages);
inputs.tools = params->tools;
inputs.tool_choice = params->tool_choice;
inputs.add_generation_prompt = true;

auto result = common_chat_templates_apply(chat_templates.get(), inputs);
state->prompt = result.prompt;
sampler->grammar = result.grammar;  // Apply grammar constraints
```

### 5.3 Message Format Conversion

Create adapter functions to convert between formats:

```cpp
// In llamafile/server/chat_adapter.h
namespace lf {
namespace server {

// Convert llamafile's llama_chat_msg to common_chat_msg
common_chat_msg to_common_chat_msg(const llama_chat_msg& msg);

// Convert common_chat_msg back for response formatting
Json format_common_chat_msg(const common_chat_msg& msg);

} // namespace server
} // namespace lf
```

---

## Part 6: Risk Analysis & Mitigation

### Risk 1: Binary Size Increase

**Impact:** Copying all chat.cpp code may significantly increase binary size

**Mitigation:**
- Use linker flags to eliminate unused code
- Consider only including parsers for popular formats initially
- Profile and optimize

### Risk 2: Build Complexity

**Impact:** Adding nlohmann/json and minja increases build complexity

**Mitigation:**
- Both are header-only, minimal build changes
- Can vendor headers directly in llamafile tree
- Document build process clearly

### Risk 3: Maintenance Burden

**Impact:** Divergence from llama.cpp implementations over time

**Mitigation:**
- Periodic sync process
- Clearly mark vendored code with origin
- Consider upstreaming improvements back to llama.cpp

### Risk 4: Streaming Complexity

**Impact:** Tool call parsing during streaming is complex

**Mitigation:**
- llama.cpp's implementation is well-tested
- Use existing diff computation logic
- Thorough testing of streaming edge cases

---

## Part 7: File-by-File Migration Checklist

```
PHASE 1: Foundation
==========================================================================================
[x] Copy vendor/minja/minja.hpp -> llamafile/server/minja.hpp
[ ] Copy vendor/minja/chat-template.hpp -> llamafile/server/chat-template.hpp
[ ] Copy common/json-schema-to-grammar.h -> llamafile/server/json-schema-to-grammar.h
[ ] Copy common/json-schema-to-grammar.cpp -> llamafile/server/json-schema-to-grammar.cpp
[ ] Copy common/chat.h -> llamafile/server/chat.h
[ ] Copy common/chat.cpp -> llamafile/server/chat.cpp
[ ] Update llamafile/server/BUILD.mk with new files
[ ] Verify compilation

PHASE 2: Parser Integration
==========================================================================================
[ ] Copy common/chat-parser.h -> llamafile/server/chat-parser.h
[ ] Copy common/chat-parser.cpp -> llamafile/server/chat-parser.cpp
[ ] Copy common/chat-parser-xml-toolcall.h -> llamafile/server/chat-parser-xml-toolcall.h
[ ] Copy common/chat-peg-parser.h -> llamafile/server/chat-peg-parser.h
[ ] Copy common/chat-peg-parser.cpp -> llamafile/server/chat-peg-parser.cpp
[ ] Copy common/peg-parser.h -> llamafile/server/peg-parser.h
[ ] Copy common/peg-parser.cpp -> llamafile/server/peg-parser.cpp
[ ] Copy common/regex-partial.h -> llamafile/server/regex-partial.h
[ ] Verify compilation
[ ] Test parser formats

PHASE 3: Chat Completions Integration
==========================================================================================
[ ] Modify V1ChatCompletionParams to add tools, tool_choice
[ ] Remove "not supported" errors in get_v1_chat_completions_params
[ ] Parse tools field with common_chat_tools_parse_oaicompat
[ ] Parse tool_choice field with common_chat_tool_choice_parse_oaicompat
[ ] Initialize common_chat_templates from model
[ ] Apply chat template with tools
[ ] Convert tool schemas to grammars
[ ] Apply grammar to sampler
[ ] Parse model output for tool calls
[ ] Format response with tool_calls
[ ] Implement streaming with diffs
[ ] Support "tool" role messages

PHASE 4: Testing
==========================================================================================
[ ] Write unit tests for json-schema-to-grammar
[ ] Write unit tests for each parser format
[ ] Write integration tests with real models
[ ] Test with OpenAI Python SDK
[ ] Test with LangChain
[ ] Performance benchmarking

PHASE 5: Documentation & Polish
==========================================================================================
[ ] Update API documentation
[ ] Add tool call examples
[ ] Document supported formats
[ ] Add CLI flags for tool settings
[ ] Final testing and validation
```

---

## Part 8: Code Examples

### Example 1: Minimal Tool Call Implementation (Conceptual)

```cpp
// llamafile/server/v1_chat_completions_tool.cpp (NEW FILE)

namespace lf {
namespace server {

bool Client::get_v1_chat_completions_params_with_tools(
    V1ChatCompletionParams* params) {

    // ... existing parsing code ...

    // NEW: Parse tools field
    if (json.contains("tools")) {
        try {
            auto tools_json = json["tools"];
            params->tools = common_chat_tools_parse_oaicompat(tools_json);
        } catch (const std::exception& e) {
            return send_error(400, std::string("Invalid tools: ") + e.what());
        }
    }

    // NEW: Parse tool_choice field
    if (json.contains("tool_choice")) {
        auto tc = json["tool_choice"];
        if (tc.is_string()) {
            params->tool_choice =
                common_chat_tool_choice_parse_oaicompat(tc.getString());
        } else if (tc.isObject()) {
            // Handle {type: "function", name: "..."} format
            params->tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;
            // ... specific tool selection ...
        }
    }

    // NEW: Parse parallel_tool_calls
    if (json.contains("parallel_tool_calls")) {
        params->parallel_tool_calls = json["parallel_tool_calls"].getBool();
    }

    return true;
}

bool Client::v1_chat_completions_with_tools() {
    auto params = new V1ChatCompletionParams;
    // ... get params ...

    // NEW: Initialize chat templates from model
    auto chat_templates = common_chat_templates_init(
        model_, FLAG_chat_template);

    // NEW: Build template inputs with tools
    common_chat_templates_inputs inputs;
    inputs.messages = convert_to_common_chat_msgs(params->messages);
    inputs.tools = params->tools;
    inputs.tool_choice = params->tool_choice;
    inputs.add_generation_prompt = true;

    // NEW: Apply template with tools
    auto result = common_chat_templates_apply(
        chat_templates.get(), inputs);

    state->prompt = result.prompt;

    // NEW: Apply grammar constraints from tool schemas
    if (!result.grammar.empty()) {
        params->grammar = result.grammar;
    }

    // ... rest of generation logic ...

    // NEW: Parse output for tool calls
    common_chat_syntax syntax;
    syntax.format = detect_model_format(model_);

    common_chat_msg parsed = common_chat_parse(
        state->generated_text,
        /*is_partial=*/false,
        syntax);

    // NEW: Format response with tool_calls
    Json response = parsed.to_json_oaicompat();
    if (!parsed.tool_calls.empty()) {
        choice["finish_reason"] = "tool_calls";
    }

    // ... send response ...
}

} // namespace server
} // namespace lf
```

### Example 2: Handling Streaming Tool Calls

```cpp
// Streaming with tool call delta updates

std::string last_content;
std::vector<common_chat_tool_call> last_tool_calls;
std::string generated_text;

// During generation loop:
for (;;) {
    // ... generate token ...
    generated_text += token_text;

    // Parse current state
    common_chat_msg current = common_chat_parse(
        generated_text,
        /*is_partial=*/true,  // PARTIAL!
        syntax);

    // Compute diffs
    auto diffs = common_chat_msg_diff::compute_diffs(
        { .content = last_content, .tool_calls = last_tool_calls },
        current);

    // Send each diff as SSE event
    for (const auto& diff : diffs) {
        Json delta = common_chat_msg_diff_to_json_oaicompat(diff);
        send_sse_delta(delta);
    }

    // Update last state
    last_content = current.content;
    last_tool_calls = current.tool_calls;
}
```

---

## Part 9: Success Criteria

### Functional Requirements
- [ ] Accept `tools` array in /v1/chat/completions request
- [ ] Accept `tool_choice` parameter (auto, none, required, specific)
- [ ] Generate tool calls following OpenAI format
- [ ] Support `parallel_tool_calls` for models that support it
- [ ] Parse tool call results from `role: "tool"` messages
- [ ] Support streaming responses with tool call deltas
- [ ] Set `finish_reason: "tool_calls"` when appropriate

### Model Support
- [ ] Llama 3.1 / 3.2 tool calling
- [ ] DeepSeek R1 tool calling
- [ ] Mistral NeMo tool calling
- [ ] Generic JSON format for other models

### Compatibility
- [ ] OpenAI Python SDK compatibility
- [ ] LangChain tool calling compatibility
- [ ] Backward compatible with existing non-tool requests

### Performance
- [ ] < 50ms overhead for tool call parsing
- [ ] < 10% binary size increase (with linker optimization)

---

## Part 10: Open Questions

1. **JSON library:** Should we adapt chat.cpp to use fastjson instead of nlohmann/json?
   - *Recommendation:* Use nlohmann/json for consistency with llama.cpp

2. **minja integration:** Should we use minja for all chat templates or keep using llama.cpp C API for non-tool requests?
   - *Recommendation:* Use minja for all requests to maintain consistency

3. **Parser selection:** How should we detect which parser format to use?
   - *Recommendation:* Use GGUF metadata with fallback to generic format

4. **Grammar triggers:** Should we implement `grammar_triggers` for dynamic grammar switching?
   - *Recommendation:* Yes, needed for models like Granite that switch formats

5. **Streaming:** How to handle partial tool calls during streaming?
   - *Recommendation:* Use llama.cpp's existing diff computation logic

---

## Appendix A: References

- [llama.cpp server implementation](https://github.com/ggerganov/llama.cpp/tree/master/tools/server)
- [OpenAI function calling documentation](https://platform.openai.com/docs/guides/function-calling)
- [llama.cpp chat.h](https://github.com/ggerganov/llama.cpp/blob/master/common/chat.h)
- [minja template engine](https://github.com/google/minja)

---

## Appendix B: Contact & Review

**Document Author:** Claude (Anthropic)
**Reviewers:** TBD
**Approval:** TBD

**Change Log:**
| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 1.0 | 2025-12-26 | Initial design document | Claude |