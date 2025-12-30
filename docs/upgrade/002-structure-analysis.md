# Migration Log #002: Detailed Structure Analysis

## Date
2025-12-28

## Purpose
Document the detailed structural differences between old and new llama.cpp for updating the build system.

## Old vs New Structure Mapping

### 1. llama.cpp Core Files

| Old Location | New Location |
|--------------|--------------|
| `src/llama.cpp` | `src/llama.cpp` (still exists but is now smaller) |
| `src/llama.cpp` | `src/llama-adapter.cpp` |
| `src/llama.cpp` | `src/llama-arch.cpp` |
| `src/llama.cpp` | `src/llama-batch.cpp` |
| `src/llama.cpp` | `src/llama-chat.cpp` |
| `src/llama.cpp` | `src/llama-context.cpp` |
| `src/llama.cpp` | `src/llama-cparams.cpp` |
| `src/llama.cpp` | `src/llama-grammar.cpp` |
| `src/llama.cpp` | `src/llama-graph.cpp` |
| `src/llama.cpp` | `src/llama-hparams.cpp` |
| `src/llama.cpp` | `src/llama-impl.cpp` |
| `src/llama.cpp` | `src/llama-io.cpp` |
| `src/llama.cpp` | `src/llama-kv-cache.cpp` |
| `src/llama.cpp` | `src/llama-kv-cache-iswa.cpp` |
| `src/llama.cpp` | `src/llama-memory.cpp` |
| `src/llama.cpp` | `src/llama-memory-hybrid.cpp` |
| `src/llama.cpp` | `src/llama-memory-recurrent.cpp` |
| `src/llama.cpp` | `src/llama-mmap.cpp` |
| `src/llama.cpp` | `src/llama-model.cpp` |
| `src/llama.cpp` | `src/llama-model-loader.cpp` |
| `src/llama.cpp` | `src/llama-model-saver.cpp` |
| `src/llama.cpp` | `src/llama-quant.cpp` |
| `src/llama.cpp` | `src/llama-sampling.cpp` |
| `src/llama.cpp` | `src/llama-vocab.cpp` |

### 2. Header Files

| Old Location | New Location |
|--------------|--------------|
| `include/llama.h` | `include/llama.h` (still exists) |
| `include/llama.h` | `include/llama-cpp.h` (new) |
| `src/llama-*.h` | `src/llama-*.h` (individual headers for each module) |

### 3. Common Files

| Old Location | New Location |
|--------------|--------------|
| `common/common.cpp` | `common/common.cpp` (unchanged) |
| `common/common.h` | `common/common.h` (unchanged) |
| `common/json.hpp` | `common/json.hpp` (unchanged) |
| `common/json-schema-to-grammar.cpp` | `common/json-schema-to-grammar.cpp` (unchanged) |
| N/A | `common/arg.cpp` (new) |
| N/A | `common/arg.h` (new) |
| N/A | `common/download.cpp` (new) |
| N/A | `common/http.h` (new) |
| N/A | `common/json-partial.cpp` (new) |
| N/A | `common/llguidance.cpp` (new) |
| N/A | `common/chat-parser-xml-toolcall.cpp` (new) |

### 4. Tools/Applications

| Old Location | New Location |
|--------------|--------------|
| `examples/main/main.cpp` | `tools/cli/cli.cpp` |
| `examples/server/server.cpp` | `tools/server/` (modularized into 8+ files) |
| `examples/llava/` | `tools/mtmd/` (multimodal tools directory) |
| `examples/llama-bench/` | `tools/llama-bench/` |
| `examples/imatrix/` | `tools/imatrix/` |
| `examples/quantize/` | `tools/quantize/` |
| `examples/perplexity/` | `tools/perplexity/` |
| N/A | `tools/completion/` (new) |
| N/A | `tools/export-lora/` (new) |
| N/A | `tools/fit-params/` (new) |
| N/A | `tools/gguf-split/` (new) |
| N/A | `tools/rpc/` (new) |
| N/A | `tools/run/` (new) |
| N/A | `tools/tokenize/` (new) |
| N/A | `tools/tts/` (new) |
| N/A | `tools/batched-bench/` (new) |
| N/A | `tools/cvector-generator/` (new) |

### 5. GGML Files

| Old Location | New Location |
|--------------|--------------|
| `ggml/src/ggml.c` | `ggml/src/ggml.cu` (now CUDA) or `ggml/src/ggml-common.c` |
| `ggml/include/ggml.h` | `ggml/include/ggml.h` (unchanged) |
| `ggml/src/ggml-alloc.c` | `ggml/src/ggml-alloc.c` (unchanged) |
| `ggml/src/ggml-backend.c` | `ggml/src/ggml-backend.cpp` (now C++) |
| N/A | `ggml/src/ggml-backend-reg.cpp` (new) |
| N/A | `ggml/src/ggml-opt.cpp` (new) |
| N/A | `ggml/src/ggml-threading.cpp` (new) |

## Updated renames.sh Pattern

The new `renames.sh` needs to handle:

### Common Files (flatten from `common/`)
```bash
# Most common/ files stay at root, same as before
mv common/common.cpp common.cpp
mv common/common.h common.h
mv common/json.hpp json.h
# ... etc
```

### Core llama.cpp Files (NEW - 24+ files)
```bash
# Move all llama-*.cpp and llama-*.h from src/ to root
mv src/llama.cpp llama.cpp
mv src/llama-adapter.cpp llama-adapter.cpp
mv src/llama-adapter.h llama-adapter.h
mv src/llama-arch.cpp llama-arch.cpp
mv src/llama-arch.h llama-arch.h
# ... and so on for all 24+ files
```

### Tools Directory
```bash
# Old: examples/main/ -> main/
# New: tools/cli/ -> cli/
mkdir -p cli
mv tools/cli/cli.cpp cli/cli.cpp

# Old: examples/server/ -> server/
# New: tools/server/ -> server/
mkdir -p server/public server/themes
mv tools/server/*.cpp server/
mv tools/server/*.h server/
mv tools/server/public/* server/public/
# ... etc

# Old: examples/llava/ -> llava/
# New: tools/mtmd/ -> llava/
mkdir -p llava
mv tools/mtmd/clip.cpp llava/clip.cpp
mv tools/mtmd/clip.h llava/clip.h
mv tools/mtmd/mtmd-cli.cpp llava/llava-cli.cpp
mv tools/mtmd/mtmd.cpp llava/llava.cpp
mv tools/mtmd/mtmd.h llava/llava.h
# ... etc
```

### GGML Files
```bash
# ggml structure changed - ggml.c is now split into multiple files
mv ggml/src/ggml-common.c ggml-common.c
mv ggml/src/ggml-alloc.c ggml-alloc.c
mv ggml/include/ggml-alloc.h ggml-alloc.h
mv ggml/src/ggml-backend.cpp ggml-backend.cpp
mv ggml/include/ggml-backend.h ggml-backend.h
mv ggml/src/ggml-opt.cpp ggml-opt.cpp
mv ggml/include/ggml-opt.h ggml-opt.h
# ... etc
```

## Key Observations

1. **Modularization**: The single `llama.cpp` file was split into 24+ specialized modules
2. **C++ Migration**: Some `.c` files became `.cpp` files (e.g., `ggml-backend.c` → `ggml-backend.cpp`)
3. **Directory Renaming**: `examples/` → `tools/`, `examples/llava/` → `tools/mtmd/`
4. **New Common Files**: Several new common files for arg parsing, download, HTTP, etc.
5. **GGML Changes**: New files like `ggml-opt.cpp`, `ggml-threading.cpp`, `ggml-backend-reg.cpp`

## Next Steps

1. Search llama.cpp git history for the exact commits/PRs that caused these changes
2. Create updated `renames.sh` for the new structure
3. Update `BUILD.mk` generation to include all new source files
