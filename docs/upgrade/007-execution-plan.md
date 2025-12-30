# Execution Plan: llama.cpp Native Structure Integration

## Strategy: Minimal Stub Redirects

Create stub headers in `llama.cpp/` root that redirect to canonical locations. This keeps submodule changes minimal while satisfying mkdeps.

## Files to Create (Stubs in llama.cpp/)

### Core llama.cpp headers (needed by common code):
```bash
# llama.h -> include/llama.h (already exists upstream)
# llama-grammar.h -> include/llama-grammar.h (already exists upstream)
# grammar-parser.h -> common/grammar-parser.h
# unicode.h -> src/unicode.h
# unicode-data.h -> src/unicode-data.h
# llama-sampling.h -> common/sampling.h
# llama-kv-cache.h -> src/llama-kv-cache.h
# ngram-cache.h -> common/ngram-cache.h
# common.h -> common/common.h
# log.h -> common/log.h
# arg.h -> common/arg.h
# console.h -> common/console.h
# json-schema-to-grammar.h -> common/json-schema-to-grammar.h
# peg-parser.h -> common/peg-parser.h
# regex-partial.h -> common/regex-partial.h
# sampling.h -> common/sampling.h
# speculative.h -> common/speculative.h
# chat.h -> common/chat.h
# chat-parser.h -> common/chat-parser.h
# chat-parser-xml-toolcall.h -> common/chat-parser-xml-toolcall.h
# chat-peg-parser.h -> common/chat-peg-parser.h
# http.h -> common/http.h
# base64.h -> common/base64.hpp
# download.h -> common/download.h
```

### GGML headers (needed by llamafile):
```bash
# ggml.h -> ggml/include/ggml.h
# ggml-alloc.h -> ggml/include/ggml-alloc.h
# ggml-backend.h -> ggml/include/ggml-backend.h
# ggml-opt.h -> ggml/include/ggml-opt.h
# ggml-cpu.h -> ggml/include/ggml-cpu.h
# gguf.h -> ggml/include/gguf.h
# ggml-quants.h -> ggml/src/ggml-quants.h
# ggml-impl.h -> ggml/src/ggml-impl.h
# ggml-common.h -> ggml/src/ggml-common.h
# ggml-cpu-impl.h -> ggml/src/ggml-cpu/ggml-cpu-impl.h
# ggml-backend-impl.h -> ggml/src/ggml-backend-impl.h
# ggml-metal.h -> ggml/src/ggml-metal/ggml-metal-impl.h
```

### MTML headers (vision):
```bash
# mtmd.h -> tools/mtmd/mtmd.h
# clip.h -> tools/mtmd/clip.h
# clip-graph.h -> tools/mtmd/clip-graph.h
```

### Vendor headers:
```bash
# json.hpp -> vendor/nlohmann/json.hpp
# json_fwd.hpp -> vendor/nlohmann/json_fwd.hpp
```

## Files to Modify (llama.cpp/common/)

### Add GGML_MULTIPLATFORM support:
- `common.cpp` - cache directory path
- `arg.cpp` - PATH_MAX include
- `download.cpp` - PATH_MAX include

## Files to Modify (llamafile/)

### Already done:
- API migrations (vocab, memory, batch)
- strlib.h rename
- Server files updated

### Still needed:
- Verify all includes use correct paths
- Fix any remaining API issues

## BUILD.mk Updates

1. Update source paths to use native structure
2. Add new arch-specific quants files
3. Remove references to deleted files
4. Update include paths

## Step-by-Step Execution

1. Create stub headers in llama.cpp/ root
2. Add GGML_MULTIPLATFORM support in llama.cpp/common/
3. Update BUILD.mk for native structure
4. Fix any remaining compilation issues
5. Test build
6. Verify runtime functionality
