#!/bin/bash
# Tool Call Support Migration Script for Llamafile
# This script copies necessary files from llama.cpp to enable tool call support
#
# Usage:
#   ./tool-call-migration-script.sh [--phase N] [--dry-run]
#
# Phases:
#   1 - Foundation (minja, core chat types, partial parsing)
#   2 - Chat Parsers (all format-specific parsers)
#   3 - Server Integration (patches and integration)
#   all - All phases

set -euo pipefail

# Configuration
LLAMA_CPP_SRC="${LLAMA_CPP_SRC:-../llama.cpp}"
LLAMAFILE_DST="${LLAMAFILE_DST:-.}"
DRY_RUN=false
PHASE="all"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

copy_file() {
    local src="$1"
    local dst="$2"
    if [ "$DRY_RUN" = true ]; then
        echo "[DRY-RUN] cp $src -> $dst"
    else
        mkdir -p "$(dirname "$dst")"
        cp "$src" "$dst"
        log_info "Copied: $src -> $dst"
    fi
}

copy_dir() {
    local src="$1"
    local dst="$2"
    if [ "$DRY_RUN" = true ]; then
        echo "[DRY-RUN] cp -r $src -> $dst"
    else
        mkdir -p "$dst"
        cp -r "$src"/* "$dst"/
        log_info "Copied directory: $src -> $dst"
    fi
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --phase)
            PHASE="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --llama-cpp)
            LLAMA_CPP_SRC="$2"
            shift 2
            ;;
        --llamafile)
            LLAMAFILE_DST="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--phase N|all] [--dry-run] [--llama-cpp PATH] [--llamafile PATH]"
            echo ""
            echo "Phases:"
            echo "  1   - Foundation (minja, core chat types)"
            echo "  2   - Chat Parsers (format-specific parsers)"
            echo "  3   - Server Integration"
            echo "  all - All phases (default)"
            exit 0
            ;;
        *)
            log_error "Unknown argument: $1"
            exit 1
            ;;
    esac
done

# Verify source directory
if [ ! -d "$LLAMA_CPP_SRC/common" ]; then
    log_error "llama.cpp source not found at: $LLAMA_CPP_SRC"
    log_error "Please set LLAMA_CPP_SRC or use --llama-cpp PATH"
    exit 1
fi

log_info "Source: $LLAMA_CPP_SRC"
log_info "Destination: $LLAMAFILE_DST"
log_info "Phase: $PHASE"
[ "$DRY_RUN" = true ] && log_warn "DRY RUN MODE - no files will be copied"

echo ""

################################################################################
# PHASE 1: Foundation
################################################################################
phase1_foundation() {
    log_info "========== PHASE 1: Foundation =========="

    # Create vendor/minja directory
    log_info "Copying minja template engine..."
    copy_dir "$LLAMA_CPP_SRC/vendor/minja" "$LLAMAFILE_DST/llama.cpp/vendor/minja"

    # Core chat types and implementation
    log_info "Copying core chat files..."
    copy_file "$LLAMA_CPP_SRC/common/chat.h" "$LLAMAFILE_DST/llama.cpp/common/chat.h"
    copy_file "$LLAMA_CPP_SRC/common/chat.cpp" "$LLAMAFILE_DST/llama.cpp/common/chat.cpp"

    # Partial JSON parsing (for streaming)
    log_info "Copying partial JSON parsing..."
    copy_file "$LLAMA_CPP_SRC/common/json-partial.h" "$LLAMAFILE_DST/llama.cpp/common/json-partial.h"
    copy_file "$LLAMA_CPP_SRC/common/json-partial.cpp" "$LLAMAFILE_DST/llama.cpp/common/json-partial.cpp"

    # Partial regex matching
    log_info "Copying partial regex..."
    copy_file "$LLAMA_CPP_SRC/common/regex-partial.h" "$LLAMAFILE_DST/llama.cpp/common/regex-partial.h"
    copy_file "$LLAMA_CPP_SRC/common/regex-partial.cpp" "$LLAMAFILE_DST/llama.cpp/common/regex-partial.cpp"

    # PEG parser
    log_info "Copying PEG parser..."
    copy_file "$LLAMA_CPP_SRC/common/peg-parser.h" "$LLAMAFILE_DST/llama.cpp/common/peg-parser.h"
    copy_file "$LLAMA_CPP_SRC/common/peg-parser.cpp" "$LLAMAFILE_DST/llama.cpp/common/peg-parser.cpp"

    # Chat PEG parser
    log_info "Copying chat PEG parser..."
    copy_file "$LLAMA_CPP_SRC/common/chat-peg-parser.h" "$LLAMAFILE_DST/llama.cpp/common/chat-peg-parser.h"
    copy_file "$LLAMA_CPP_SRC/common/chat-peg-parser.cpp" "$LLAMAFILE_DST/llama.cpp/common/chat-peg-parser.cpp"

    log_info "Phase 1 complete!"
}

################################################################################
# PHASE 2: Chat Parsers
################################################################################
phase2_chat_parsers() {
    log_info "========== PHASE 2: Chat Parsers =========="

    # Main chat parser
    log_info "Copying main chat parser..."
    copy_file "$LLAMA_CPP_SRC/common/chat-parser.h" "$LLAMAFILE_DST/llama.cpp/common/chat-parser.h"
    copy_file "$LLAMA_CPP_SRC/common/chat-parser.cpp" "$LLAMAFILE_DST/llama.cpp/common/chat-parser.cpp"

    # XML tool call parser
    log_info "Copying XML tool call parser..."
    copy_file "$LLAMA_CPP_SRC/common/chat-parser-xml-toolcall.h" "$LLAMAFILE_DST/llama.cpp/common/chat-parser-xml-toolcall.h"
    copy_file "$LLAMA_CPP_SRC/common/chat-parser-xml-toolcall.cpp" "$LLAMAFILE_DST/llama.cpp/common/chat-parser-xml-toolcall.cpp"

    # Internal header
    log_info "Copying internal header..."
    copy_file "$LLAMA_CPP_SRC/common/chat-parsers-internal.h" "$LLAMAFILE_DST/llama.cpp/common/chat-parsers-internal.h"

    # Model-specific parsers
    log_info "Copying model-specific parsers..."
    mkdir -p "$LLAMAFILE_DST/llama.cpp/common/chat-parsers"

    for parser in "$LLAMA_CPP_SRC"/common/chat-parsers/*.cpp; do
        if [ -f "$parser" ]; then
            basename=$(basename "$parser")
            copy_file "$parser" "$LLAMAFILE_DST/llama.cpp/common/chat-parsers/$basename"
        fi
    done

    log_info "Phase 2 complete!"
}

################################################################################
# PHASE 3: Server Integration (Creates patch templates)
################################################################################
phase3_server_integration() {
    log_info "========== PHASE 3: Server Integration =========="

    log_warn "Phase 3 requires manual integration. See generated files."

    # Create a template for the server integration
    if [ "$DRY_RUN" = false ]; then
        mkdir -p "$LLAMAFILE_DST/docs/tool-call-integration"

        # Generate integration guide
        cat > "$LLAMAFILE_DST/docs/tool-call-integration/README.md" << 'EOF'
# Server Integration for Tool Call Support

This directory contains guidance for integrating tool call support into the llamafile server.

## Files to Modify

### 1. Remove Tool Rejection (Priority: High)

**File:** `llama.cpp.patches/patches/server_utils.h.patch`

Remove or modify the lines that reject `tools` and `tool_choice` parameters:

```cpp
// REMOVE THESE LINES:
static const std::vector<std::string> unsupported_params { "tools", "tool_choice" };
for (auto & param : unsupported_params) {
    if (body.contains(param)) {
        throw std::runtime_error("Unsupported param: " + param);
    }
}
```

### 2. Add Chat Templates Initialization (server.cpp)

Add to server context initialization:
```cpp
#include "llama.cpp/common/chat.h"

// In llama_server_context:
common_chat_templates_ptr chat_templates;

// In initialization:
chat_templates = common_chat_templates_init(model, chat_template, "", "");
```

### 3. Update Request Parsing (utils.h or oai.h)

Replace `format_chat()` with tool-aware version:
```cpp
#include "llama.cpp/common/chat.h"

static json oaicompat_completion_params_parse(...) {
    // Parse tools
    std::vector<common_chat_tool> tools;
    if (body.contains("tools")) {
        tools = common_chat_tools_parse_oaicompat(body.at("tools"));
    }

    // Parse tool_choice
    common_chat_tool_choice tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;
    if (body.contains("tool_choice")) {
        auto tc = body.at("tool_choice");
        if (tc.is_string()) {
            tool_choice = common_chat_tool_choice_parse_oaicompat(tc.get<std::string>());
        }
    }

    // Create inputs
    common_chat_templates_inputs inputs;
    inputs.messages = common_chat_msgs_parse_oaicompat(body.at("messages"));
    inputs.tools = tools;
    inputs.tool_choice = tool_choice;
    inputs.add_generation_prompt = true;

    // Apply template
    auto chat_params = common_chat_templates_apply(ctx.chat_templates.get(), inputs);

    llama_params["prompt"] = chat_params.prompt;
    if (!chat_params.grammar.empty()) {
        llama_params["grammar"] = chat_params.grammar;
    }

    // Store syntax for response parsing
    llama_params["__chat_syntax"] = /* serialize chat_params syntax */;
}
```

### 4. Add Response Parsing (server.cpp)

In `send_final_response()`:
```cpp
if (slot.oaicompat && slot.oaicompat_chat_syntax.parse_tool_calls) {
    auto parsed = common_chat_parse(
        slot.generated_text,
        false,  // not partial
        slot.oaicompat_chat_syntax);

    if (!parsed.tool_calls.empty()) {
        json tool_calls_json = json::array();
        for (const auto& tc : parsed.tool_calls) {
            tool_calls_json.push_back({
                {"id", tc.id},
                {"type", "function"},
                {"function", {
                    {"name", tc.name},
                    {"arguments", tc.arguments}
                }}
            });
        }
        res["choices"][0]["message"]["tool_calls"] = tool_calls_json;
        res["choices"][0]["message"]["content"] = nullptr;
        res["choices"][0]["finish_reason"] = "tool_calls";
    }
}
```

### 5. Add Streaming Support (server.cpp)

In `send_partial_response()`:
```cpp
if (slot.oaicompat && slot.oaicompat_chat_syntax.parse_tool_calls) {
    auto new_msg = common_chat_parse(
        slot.generated_text,
        true,  // partial
        slot.oaicompat_chat_syntax);

    auto diffs = common_chat_msg_diff::compute_diffs(
        slot.prev_parsed_msg,
        new_msg);

    // Send tool call deltas
    for (const auto& diff : diffs) {
        if (diff.tool_call_index != std::string::npos) {
            // Stream tool call update
        }
    }

    slot.prev_parsed_msg = new_msg;
}
```

## Build System Changes

### llama.cpp/BUILD.mk

Add new source files:
```makefile
LLAMA_CPP_COMMON_SRCS += \
    llama.cpp/common/chat.cpp \
    llama.cpp/common/chat-parser.cpp \
    llama.cpp/common/chat-parser-xml-toolcall.cpp \
    llama.cpp/common/chat-peg-parser.cpp \
    llama.cpp/common/json-partial.cpp \
    llama.cpp/common/regex-partial.cpp \
    llama.cpp/common/peg-parser.cpp

LLAMA_CPP_COMMON_SRCS += $(wildcard llama.cpp/common/chat-parsers/*.cpp)

# Add minja to include paths
LLAMA_CPP_CXXFLAGS += -I$(srcdir)/llama.cpp/vendor/minja
```
EOF

        log_info "Created integration guide at: docs/tool-call-integration/README.md"
    fi

    log_info "Phase 3 complete (manual steps required)!"
}

################################################################################
# PHASE 4: Build System Updates
################################################################################
phase4_build_system() {
    log_info "========== PHASE 4: Build System Updates =========="

    if [ "$DRY_RUN" = false ]; then
        # Generate BUILD.mk additions
        cat > "$LLAMAFILE_DST/docs/tool-call-integration/BUILD.mk.additions" << 'EOF'
# Tool Call Support - Add to llama.cpp/BUILD.mk

# New common sources for tool call support
LLAMA_CPP_TOOLCALL_SRCS := \
    llama.cpp/common/chat.cpp \
    llama.cpp/common/chat-parser.cpp \
    llama.cpp/common/chat-parser-xml-toolcall.cpp \
    llama.cpp/common/chat-peg-parser.cpp \
    llama.cpp/common/json-partial.cpp \
    llama.cpp/common/regex-partial.cpp \
    llama.cpp/common/peg-parser.cpp \
    $(wildcard llama.cpp/common/chat-parsers/*.cpp)

LLAMA_CPP_TOOLCALL_OBJS := $(LLAMA_CPP_TOOLCALL_SRCS:%.cpp=o/$(MODE)/%.o)

# Add to include paths
LLAMA_CPP_CXXFLAGS += -I$(srcdir)/llama.cpp/vendor/minja

# Add objects to library
o/$(MODE)/llama.cpp/llama.cpp.a: $(LLAMA_CPP_TOOLCALL_OBJS)

# Compile rules for new files
$(LLAMA_CPP_TOOLCALL_OBJS): llama.cpp/BUILD.mk
EOF

        log_info "Created BUILD.mk additions at: docs/tool-call-integration/BUILD.mk.additions"
    fi

    log_info "Phase 4 complete!"
}

################################################################################
# Main Execution
################################################################################
main() {
    case "$PHASE" in
        1)
            phase1_foundation
            ;;
        2)
            phase2_chat_parsers
            ;;
        3)
            phase3_server_integration
            ;;
        4)
            phase4_build_system
            ;;
        all)
            phase1_foundation
            echo ""
            phase2_chat_parsers
            echo ""
            phase3_server_integration
            echo ""
            phase4_build_system
            ;;
        *)
            log_error "Unknown phase: $PHASE"
            exit 1
            ;;
    esac

    echo ""
    log_info "=========================================="
    log_info "Migration complete!"
    log_info ""
    log_info "Next steps:"
    log_info "  1. Review copied files for any needed adaptations"
    log_info "  2. Update BUILD.mk with new source files"
    log_info "  3. Apply server integration changes (see docs/tool-call-integration/)"
    log_info "  4. Run: make -j"
    log_info "  5. Test with: ./llamafile --server -m model.gguf"
    log_info "=========================================="
}

main
