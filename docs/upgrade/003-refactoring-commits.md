# Migration Log #003: Key llama.cpp Refactoring Commits

## Date
2025-12-28

## Purpose
Document the specific commits and PRs in llama.cpp that caused the major structural changes, to understand what needs to be adjusted in llamafile's build system.

## Version Comparison

| Item | Old Version | New Version |
|------|-------------|-------------|
| Commit | `8b3befc0e2ed8fb18b903735831496b8b0c80949` | `06705fdcb...` (upstream master) |
| Date | ~2024 | ~2025-12 |
| Structure | Old monolithic structure | New modular structure |

## Key Refactoring Commits

### 1. Directory Rename: examples → tools

**Commit**: `1d36b367`
**PR**: #13249
**Message**: "llama : move end-user examples to tools directory"
**Author**: Diego Devesa
**Date**: 2025-05-02

This commit renamed the entire `examples/` directory to `tools/`:

- `examples/main/` → `tools/cli/` (renamed to cli, not main!)
- `examples/server/` → `tools/server/`
- `examples/llava/` → `tools/mtmd/` (multimodal tools)
- `examples/llama-bench/` → `tools/llama-bench/`
- `examples/imatrix/` → `tools/imatrix/`
- `examples/quantize/` → `tools/quantize/`
- `examples/perplexity/` → `tools/perplexity/`
- `examples/batched-bench/` → `tools/batched-bench/`
- `examples/export-lora/` → `tools/export-lora/`
- `examples/gguf-split/` → `tools/gguf-split/`
- `examples/cvector-generator/` → `tools/cvector-generator/`

Additionally, new tools were added:
- `tools/completion/` (new)
- `tools/fit-params/` (new)
- `tools/rpc/` (new)
- `tools/run/` (new)
- `tools/tokenize/` (new)
- `tools/tts/` (new)

### 2. Server Modularization

**Commit**: `b8372eec`
**PR**: #17362
**Message**: "server: split server.cpp code into server/common/task/queue"
**Author**: Xuan-Son Nguyen
**Date**: 2025-11-24

This commit split the monolithic `server.cpp` into multiple files:
- `server.cpp` (main entry point, now much smaller)
- `server-common.cpp` / `server-common.h` (common utilities)
- `server-task.cpp` / `server-task.h` (task handling)
- `server-queue.cpp` / `server-queue.h` (queue management)

**Commit**: `0de8878c`
**PR**: #17216
**Message**: "server: split HTTP into its own interface"
**Date**: 2025-10-17

This further modularized the HTTP handling.

### 3. Multimodal (llava → mtmd) Rename

The `examples/llava/` directory was renamed to `tools/mtmd/` (multimodal tools):
- `mtmd-cli.cpp` (was `llava-cli.cpp`)
- `mtmd.cpp` (was `llava.cpp`)
- `mtmd.h` (was `llava.h`)
- `clip.cpp`, `clip.h` (kept same names)
- New audio support (`mtmd-audio.cpp`, `mtmd-audio.h`)

### 4. Core llama.cpp Modularization

The single `llama.cpp` file was gradually split into multiple specialized files throughout 2024-2025:

New modular files include:
- `llama-adapter.cpp` / `llama-adapter.h`
- `llama-arch.cpp` / `llama-arch.h`
- `llama-batch.cpp` / `llama-batch.h`
- `llama-chat.cpp` / `llama-chat.h`
- `llama-context.cpp` / `llama-context.h`
- `llama-cparams.cpp` / `llama-cparams.h`
- `llama-grammar.cpp` / `llama-grammar.h`
- `llama-graph.cpp` / `llama-graph.h`
- `llama-hparams.cpp` / `llama-hparams.h`
- `llama-impl.cpp` / `llama-impl.h`
- `llama-io.cpp` / `llama-io.h`
- `llama-kv-cache.cpp` / `llama-kv-cache.h`
- `llama-kv-cache-iswa.cpp` / `llama-kv-cache-iswa.h`
- `llama-memory.cpp` / `llama-memory.h`
- `llama-memory-hybrid.cpp` / `llama-memory-hybrid.h`
- `llama-memory-recurrent.cpp` / `llama-memory-recurrent.h`
- `llama-mmap.cpp` / `llama-mmap.h`
- `llama-model.cpp` / `llama-model.h`
- `llama-model-loader.cpp` / `llama-model-loader.h`
- `llama-model-saver.cpp` / `llama-model-saver.h`
- `llama-quant.cpp` / `llama-quant.h`
- `llama-sampling.cpp` / `llama-sampling.h`
- `llama-vocab.cpp` / `llama-vocab.h`

### 5. Common Directory Enhancements

New files added to `common/`:
- `arg.cpp` / `arg.h` (argument parsing)
- `download.cpp` / `download.h` (download utilities)
- `http.h` (HTTP client utilities)
- `json-partial.cpp` / `json-partial.h` (partial JSON parsing)
- `llguidance.cpp` (LLM guidance)
- `chat-parser-xml-toolcall.cpp` / `chat-parser-xml-toolcall.h` (XML tool call parsing)

### 6. GGML Changes

- Some `.c` files converted to `.cpp` (e.g., `ggml-backend.c` → `ggml-backend.cpp`)
- New files:
  - `ggml-opt.cpp` / `ggml-opt.h`
  - `ggml-threading.cpp`
  - `ggml-backend-reg.cpp`

### 7. Build System Change

The Makefile was replaced with CMake-only build. The old Makefile now just shows an error message directing users to use CMake.

## Impact on llamafile Build System

### Files That Need Updates

1. **`llama.cpp.patches/renames.sh`** - Needs complete rewrite for:
   - `tools/` instead of `examples/`
   - `tools/cli/` instead of `examples/main/`
   - `tools/mtmd/` instead of `examples/llava/`
   - 24+ `llama-*.cpp` and `llama-*.h` files from `src/`

2. **`llama.cpp.patches/llamafile-files/BUILD.mk`** - Needs to:
   - Build 24+ `llama-*.cpp` files instead of single `llama.cpp`
   - Include new common files (`arg.cpp`, `download.cpp`, etc.)
   - Handle tools directory structure changes

3. **`llama.cpp.patches/apply-patches.sh`** - May need updates for:
   - New directory structure cleanup
   - Handling new file locations

### Estimated Changes

- **renames.sh**: ~95 file moves → ~150+ file moves
- **BUILD.mk**: ~95 source files → ~150+ source files
- New include paths may be needed for compilation

## Timeline Summary

| Date | Change | Commit |
|------|--------|--------|
| 2025-05-02 | examples → tools rename | 1d36b367 (#13249) |
| 2025-11-24 | server modularization | b8372eec (#17362) |
| 2025-10-17 | HTTP interface split | 0de8878c (#17216) |
| 2024-2025 | llama.cpp file splits | Multiple commits |
| ~2024 | Makefile → CMake | ~Same period |

## Next Steps

1. Create updated `renames.sh` based on new structure
2. Generate new `BUILD.mk` with all source files
3. Update `apply-patches.sh` if needed
4. Test the build
