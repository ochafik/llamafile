# Migration Log #004: Updated renames.sh

## Date
2025-12-28

## Summary
Created updated `renames.sh` for new llama.cpp structure. Old version backed up to `renames.sh.old`.

## Changes Made

### Section 1: Common Files (28 files → 34 files)
Added new files:
- `arg.cpp/h` (argument parsing)
- `chat-parser-xml-toolcall.cpp/h` (XML tool call parsing)
- `chat-peg-parser.cpp/h` (PEG parser for chat)
- `download.cpp/h` (download utilities)
- `http.h` (HTTP client)
- `json-partial.cpp/h` (partial JSON parsing)
- `llguidance.cpp` (LLM guidance)
- `peg-parser.cpp/h` (PEG parser)
- `preset.cpp/h` (presets)
- `regex-partial.cpp/h` (regex partial matching)
- `speculative.cpp/h` (speculative decoding)

### Section 2: Core llama.cpp Files (5 files → 55 files)
**Major change**: Single `llama.cpp` split into 24+ modularized files:

New files added:
- `llama-adapter.cpp/h` (adapter handling)
- `llama-arch.cpp/h` (architecture)
- `llama-batch.cpp/h` (batch processing)
- `llama-chat.cpp/h` (chat functionality)
- `llama-context.cpp/h` (context management)
- `llama-cparams.cpp/h` (context parameters)
- `llama-graph.cpp/h` (computation graph)
- `llama-hparams.cpp/h` (hyperparameters)
- `llama-io.cpp/h` (I/O operations)
- `llama-kv-cache-iswa.cpp/h` (KV cache ISWA)
- `llama-kv-cells.h` (KV cells)
- `llama-memory.cpp/h` (memory management)
- `llama-memory-hybrid.cpp/h` (hybrid memory)
- `llama-memory-recurrent.cpp/h` (recurrent memory)
- `llama-mmap.cpp/h` (memory mapping)
- `llama-model-loader.cpp/h` (model loading)
- `llama-model-saver.cpp/h` (model saving)
- `llama-quant.cpp/h` (quantization)
- `llama-sampling.cpp/h` (sampling)
- `llama-unicode.cpp/h` (unicode handling)

Kept from old:
- `llama.cpp` (main file, now smaller)
- `llama-grammar.cpp/h`
- `llama-vocab.cpp/h`
- `llama-impl.cpp/h`
- `unicode-data.cpp/h`
- `unicode.cpp/h`

Also:
- `include/llama.h` → `llama.h`
- `include/llama-cpp.h` → `llama-cpp.h` (new)

### Section 3: GGML Files (14 files → 16 files)
Added new files:
- `ggml-backend-reg.cpp` (backend registration)
- `ggml-opt.cpp/h` (GGML optimizer)
- `ggml-threading.cpp/h` (threading utilities)

Changed:
- `ggml-backend.c` → `ggml-backend.cpp` (now C++)

### Section 4: Tools/Applications
Major path changes:
- `examples/main/main.cpp` → `tools/cli/cli.cpp`
- `examples/server/server.cpp` → `tools/server/server.cpp` + 7 additional modularized files

Server files added:
- `server-common.cpp/h`
- `server-context.cpp/h`
- `server-http.cpp/h`
- `server-models.cpp/h`
- `server-queue.cpp/h`
- `server-task.cpp/h`

Server public files simplified:
- Old: `completion.js`, `index.js`, `json-schema-to-grammar.mjs`, `index.html`
- New: Only `index.html.gz`, `loading.html`

### Section 5: Multimodal/LLaVA
Path change: `examples/llava/` → `tools/mtmd/`

File renames:
- `llava-cli.cpp` → `mtmd-cli.cpp`
- `llava.cpp` → `mtmd.cpp`
- `llava.h` → `mtmd.h`

New files added:
- `mtmd-helper.cpp/h`
- `mtmd-audio.cpp/h` (audio support)
- `models/` subdirectory with 15+ model-specific files

## File Count Comparison

| Category | Old | New | Change |
|----------|-----|-----|--------|
| Common | 28 | 34 | +6 |
| Core llama | 5 | 55 | +50 |
| GGML | 14 | 16 | +2 |
| Server | 1 | 9 | +8 |
| LLaVA/mtmd | ~8 | ~25 | +17 |
| **Total** | **~95** | **~180** | **+85** |

## Next Steps

1. Update BUILD.mk to build all new source files
2. Update apply-patches.sh if needed
3. Test the renames script
4. Test the full build
