# Integrating llama.cpp Server into Llamafile

## Analysis & Migration Guide

---

## 1. Architecture Comparison

### Current Llamafile Server
```
llama.cpp/server/
├── server.cpp      (3,783 lines) - Monolithic, everything in one file
├── utils.h         (596 lines)   - Queue, helpers, chat formatting
├── oai.h           (228 lines)   - OpenAI compatibility
├── httplib.h       (8,794 lines) - HTTP library (vendored, modified)
├── macsandbox.cpp  (101 lines)   - macOS sandbox support
└── macsandbox.h    (14 lines)
```

### New llama.cpp Server (Modular)
```
tools/server/
├── server.cpp           (320 lines)  - Clean entry point
├── server-context.cpp   (3,943 lines) - Main logic, slot management
├── server-context.h     (130 lines)
├── server-common.cpp    (1,682 lines) - Request parsing, OAI compat
├── server-common.h      (363 lines)
├── server-task.cpp      (1,524 lines) - Task management, response formatting
├── server-task.h        (524 lines)
├── server-queue.cpp     (427 lines)   - Task queue
├── server-queue.h       (196 lines)
├── server-http.cpp      (400 lines)   - HTTP abstraction layer
├── server-http.h        (78 lines)
├── server-models.cpp    (980 lines)   - Multi-model router support
└── server-models.h      (194 lines)
```

**Total: ~10,761 lines** (modular) vs **~4,600 lines** (monolithic)

---

## 2. Llamafile-Specific Features to Preserve

### 2.1 Core Integrations

| Feature | Location | Description |
|---------|----------|-------------|
| **Cosmopolitan libc** | `#include <cosmo.h>` | Cross-platform binary support |
| **llamafile headers** | `llamafile/llamafile.h`, `llamafile/micros.h`, `llamafile/debug.h` | Core llamafile functionality |
| **FLAG_* globals** | Throughout | Llamafile configuration flags |
| **g_prompt_per_second_jart** | Metrics | Custom timing metric |
| **llamafile_gpu_*()** | GPU detection | GPU layer calculation |
| **llamafile_launch_browser()** | Auto-launch | Browser launch on start |
| **llamafile_trapping_enabled()** | Debug | Trap handling |

### 2.2 Security Features

| Feature | Location | Description |
|---------|----------|-------------|
| **macOS Sandbox** | `macsandbox.cpp` | Deny-by-default sandbox policy |
| **FLAG_unsecure** | Sandbox control | Skip sandbox when debugging |
| **cosmo_dlopen/dlsym** | Dynamic loading | Cosmopolitan dynamic linking |

### 2.3 Custom Arguments

```cpp
// Llamafile-specific CLI flags
--gpu GPU           // llamafile_gpu_parse()
--unsecure          // FLAG_unsecure
--fast              // FLAG_fast
--iq                // FLAG_iq
--precise           // FLAG_precise
--ascii             // FLAG_ascii
--nologo            // FLAG_nologo
--trap              // FLAG_trap
--nocompile         // FLAG_nocompile
--recompile         // FLAG_recompile
--url-prefix        // URL prefix support
```

### 2.4 Modified Behaviors

```cpp
// Public path from ZIP archive
std::string public_path = "/zip/llama.cpp/server/public";

// Embedding endpoint warnings (recommends llamafiler)
fprintf(stderr, "warning: the --embedding endpoint is no longer supported...");

// Custom GPU layer handling
params.n_gpu_layers = llamafile_gpu_layers(params.n_gpu_layers);

// Always enable embedding mode
params.embedding = true;  // [jart] #243
```

---

## 3. Integration Strategies

### Strategy A: Port New Server with Llamafile Patches

**Approach:** Take the new modular server, apply llamafile-specific modifications as patches.

```
Effort: High
Risk: Medium
Maintainability: Good (clear separation of concerns)
```

**Steps:**
1. Copy `tools/server/*.cpp/*.h` to `llama.cpp/server/`
2. Create new patch files for llamafile customizations
3. Modify entry point to include llamafile headers
4. Add macsandbox integration
5. Update BUILD.mk for new file structure

**Pros:**
- Clean architecture
- Tool calls work out of the box
- Easier to sync with upstream
- Modular code easier to maintain

**Cons:**
- Significant initial work
- Many files to patch
- Need to understand new architecture

### Strategy B: Minimal Patches to Current Server

**Approach:** Keep current server, add only tool call support.

```
Effort: Medium
Risk: Low
Maintainability: Poor (growing technical debt)
```

**Steps:**
1. Keep existing server.cpp
2. Add chat.h/cpp and parsing infrastructure
3. Modify request/response handling for tools
4. Keep all llamafile customizations in place

**Pros:**
- Minimal changes
- Lower risk
- Familiar codebase

**Cons:**
- Monolithic code grows larger
- Miss new features (router mode, Anthropic API, etc.)
- Harder to sync with upstream long-term

### Strategy C: Hybrid - New Server with Llamafile Layer

**Approach:** Use new server as library, wrap with llamafile layer.

```
Effort: Medium-High
Risk: Medium
Maintainability: Best
```

**Steps:**
1. Keep new server code largely unchanged
2. Create `llamafile-server-wrapper.cpp` that:
   - Initializes llamafile subsystems
   - Sets up sandbox
   - Configures FLAG_* from CLI
   - Calls into new server
3. Minimal patches to new server for integration points

**Pros:**
- Cleanest separation
- Easiest to sync with upstream
- New features automatically available

**Cons:**
- Need to design wrapper interface
- Some features may need hooks into server internals

---

## 4. Recommended Approach: Strategy C (Hybrid)

### 4.1 File Structure

```
llama.cpp/server/
├── server.cpp              # NEW: entry point (from tools/server/)
├── server-context.cpp      # NEW: (from tools/server/)
├── server-context.h        # NEW
├── server-common.cpp       # NEW
├── server-common.h         # NEW
├── server-task.cpp         # NEW
├── server-task.h           # NEW
├── server-queue.cpp        # NEW
├── server-queue.h          # NEW
├── server-http.cpp         # NEW (needs patches for httplib path)
├── server-http.h           # NEW
├── server-models.cpp       # NEW (optional - router mode)
├── server-models.h         # NEW
├── llamafile-integration.cpp  # NEW: llamafile-specific wrapper
├── llamafile-integration.h    # NEW
├── macsandbox.cpp          # KEEP: existing
├── macsandbox.h            # KEEP: existing
└── httplib.h               # KEEP: existing (patched)
```

### 4.2 Integration Points

#### Entry Point Wrapper (`llamafile-integration.cpp`)

```cpp
#include "llamafile/llamafile.h"
#include "llamafile/micros.h"
#include "llamafile/debug.h"
#include "macsandbox.h"
#include <cosmo.h>

#include "server-context.h"
#include "server-http.h"

// Llamafile globals
double g_prompt_per_second_jart;
bool g_server_background_mode;
llama_model *g_server_force_llama_model;
void (*g_server_on_listening)(const char *host, int port);

// Parse llamafile-specific arguments
void parse_llamafile_args(int argc, char** argv, common_params& params) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
            FLAG_gpu = llamafile_gpu_parse(argv[++i]);
        } else if (strcmp(argv[i], "--unsecure") == 0) {
            FLAG_unsecure = true;
        } else if (strcmp(argv[i], "--fast") == 0) {
            FLAG_fast = true;
        }
        // ... other llamafile flags
    }
}

// Initialize llamafile subsystems
void llamafile_server_init() {
    // GPU layer calculation
    params.n_gpu_layers = llamafile_gpu_layers(params.n_gpu_layers);
}

// Setup sandbox after model load
void llamafile_server_post_init() {
    if (!FLAG_unsecure && !g_server_background_mode) {
        if (IsXnuSilicon()) {
            std::string error;
            if (mac_sandbox_init(error) != 0) {
                fprintf(stderr, "warning: sandbox init failed: %s\n", error.c_str());
            }
        }
    }
}

// Launch browser
void llamafile_server_ready(const std::string& url) {
    if (!sparams.nobrowser) {
        llamafile_launch_browser(url.c_str());
    }
    if (g_server_on_listening) {
        g_server_on_listening(host, port);
    }
}
```

#### Patches to New Server

**server.cpp patch:**
```cpp
// Add at top
#include "llamafile-integration.h"

int main(int argc, char ** argv) {
    // Parse llamafile args first
    parse_llamafile_args(argc, argv, params);

    // Normal server init...
    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_SERVER)) {
        return 1;
    }

    // Llamafile GPU setup
    llamafile_server_init();

    // ... rest of main ...

    // After model load, before accepting connections
    llamafile_server_post_init();

    // When ready
    llamafile_server_ready(ctx_http.listening_address);
}
```

**server-http.cpp patch:**
```cpp
// Change httplib include path
#include "httplib.h"  // Use llamafile's patched version
```

**server-common.cpp patch:**
```cpp
// Add metric tracking
extern double g_prompt_per_second_jart;

// In prompt evaluation timing:
g_prompt_per_second_jart = 1e6 / (t2 - t1) * n_tokens;
```

### 4.3 BUILD.mk Changes

```makefile
LLAMA_CPP_SERVER_SRCS := \
    llama.cpp/server/server.cpp \
    llama.cpp/server/server-context.cpp \
    llama.cpp/server/server-common.cpp \
    llama.cpp/server/server-task.cpp \
    llama.cpp/server/server-queue.cpp \
    llama.cpp/server/server-http.cpp \
    llama.cpp/server/server-models.cpp \
    llama.cpp/server/llamafile-integration.cpp \
    llama.cpp/server/macsandbox.cpp

LLAMA_CPP_SERVER_OBJS := $(LLAMA_CPP_SERVER_SRCS:%.cpp=o/$(MODE)/%.o)

$(LLAMA_CPP_SERVER_OBJS): private \
    CCFLAGS += \
        -I$(srcdir)/llama.cpp/vendor/minja \
        -I$(srcdir)/llama.cpp/common \
        -I$(srcdir)/llama.cpp/server
```

---

## 5. New Features Gained

By adopting the new server, llamafile would gain:

| Feature | Description |
|---------|-------------|
| **Full Tool Calls** | OpenAI-compatible function calling |
| **Anthropic API** | `/v1/messages` endpoint |
| **Router Mode** | Multi-model serving with load balancing |
| **LoRA Hotswap** | Dynamic LoRA adapter switching |
| **Reranking** | `/v1/rerank` endpoint |
| **Apply Template** | `/apply-template` endpoint |
| **Ollama Compatibility** | `/api/chat`, `/api/tags` endpoints |
| **Streaming Tool Calls** | Partial tool call streaming |
| **Grammar-Constrained Tools** | JSON schema to GBNF |
| **25+ Model Parsers** | Llama3, Mistral, Hermes, DeepSeek, etc. |

---

## 6. Migration Checklist

### Phase 1: Setup (1 day)
- [ ] Copy `tools/server/*.cpp/*.h` to new location
- [ ] Update include paths in copied files
- [ ] Create `llamafile-integration.cpp/h`
- [ ] Update BUILD.mk

### Phase 2: Integration (2-3 days)
- [ ] Port macsandbox integration
- [ ] Port FLAG_* handling
- [ ] Port llamafile_gpu_* functions
- [ ] Port browser launch
- [ ] Port custom metrics (g_prompt_per_second_jart)

### Phase 3: Patches (1-2 days)
- [ ] Create patches for httplib path
- [ ] Create patches for llamafile headers
- [ ] Create patches for cosmopolitan compatibility
- [ ] Update apply-patches.sh

### Phase 4: Testing (2-3 days)
- [ ] Basic server startup
- [ ] Chat completions
- [ ] Tool calls with various models
- [ ] Streaming
- [ ] macOS sandbox
- [ ] GPU detection
- [ ] Browser launch

### Phase 5: Cleanup (1 day)
- [ ] Remove old server code
- [ ] Update documentation
- [ ] Update CI/CD

---

## 7. Quick Comparison

| Aspect | Current Server | New Server |
|--------|---------------|------------|
| Lines of code | ~4,600 | ~10,700 |
| Files | 6 | 14 |
| Tool call support | No | Full |
| Anthropic API | No | Yes |
| Router mode | No | Yes |
| Modularity | Monolithic | Clean separation |
| Upstream sync | Hard | Easy |
| llamafile integration | Native | Needs wrapper |

---

## 8. Recommendation

**For tool call support specifically:** Strategy B (minimal patches) is faster but creates technical debt.

**For long-term health:** Strategy C (hybrid) is recommended because:
1. New server has tool calls built-in and tested
2. Modular architecture is easier to maintain
3. Upstream sync becomes trivial
4. New features come "for free"
5. Llamafile customizations are cleanly isolated

The upfront investment of ~1-2 weeks pays off in reduced maintenance burden and automatic feature updates.
