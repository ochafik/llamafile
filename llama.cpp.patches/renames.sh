#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# llama.cpp directory flattening script for llamafile
# Updated for new llama.cpp structure (2025)
#
# This script flattens llama.cpp's directory structure to work with
# llamafile's cosmocc build system. It moves files from subdirectories
# to the root level.
#
# NOTE: We now KEEP the new upstream naming (cli, mtmd, etc.) instead
# of renaming back to old names (main, llava). Llamafile patches will be
# updated to work with the new names.
# ============================================================================

echo "Applying llama.cpp directory flattening renames..."

# ============================================================================
# Section 1: Common files (flatten from common/ to root)
# ============================================================================

echo "Flattening common/ directory..."
mv common/arg.cpp arg.cpp
mv common/arg.h arg.h
mv common/base64.hpp base64.h
mv common/chat-parser-xml-toolcall.cpp chat-parser-xml-toolcall.cpp
mv common/chat-parser-xml-toolcall.h chat-parser-xml-toolcall.h
mv common/chat-parser.cpp chat-parser.cpp
mv common/chat-parser.h chat-parser.h
mv common/chat-peg-parser.cpp chat-peg-parser.cpp
mv common/chat-peg-parser.h chat-peg-parser.h
mv common/chat.cpp chat.cpp
mv common/chat.h chat.h
mv common/common.cpp common.cpp
mv common/common.h common.h
mv common/console.cpp console.cpp
mv common/console.h console.h
mv common/download.cpp download.cpp
mv common/download.h download.h
mv common/http.h http.h
mv common/json-partial.cpp json-partial.cpp
mv common/json-partial.h json-partial.h
mv common/json.hpp json.h
mv common/json-schema-to-grammar.cpp json-schema-to-grammar.cpp
mv common/json-schema-to-grammar.h json-schema-to-grammar.h
mv common/llguidance.cpp llguidance.cpp
mv common/log.cpp log.cpp
mv common/log.h log.h
mv common/ngram-cache.cpp ngram-cache.cpp
mv common/ngram-cache.h ngram-cache.h
mv common/peg-parser.cpp peg-parser.cpp
mv common/peg-parser.h peg-parser.h
mv common/preset.cpp preset.cpp
mv common/preset.h preset.h
mv common/regex-partial.cpp regex-partial.cpp
mv common/regex-partial.h regex-partial.h
mv common/sampling.cpp sampling.cpp
mv common/sampling.h sampling.h
mv common/speculative.cpp speculative.cpp
mv common/speculative.h speculative.h
mv common/unicode.cpp unicode.cpp
mv common/unicode.h unicode.h

# ============================================================================
# Section 2: Core llama.cpp files (from src/ to root)
# New in 2025: Single llama.cpp split into 24+ modular files
# ============================================================================

echo "Flattening src/ directory (llama core files)..."
mv src/llama.cpp llama.cpp
mv src/llama-adapter.cpp llama-adapter.cpp
mv src/llama-adapter.h llama-adapter.h
mv src/llama-arch.cpp llama-arch.cpp
mv src/llama-arch.h llama-arch.h
mv src/llama-batch.cpp llama-batch.cpp
mv src/llama-batch.h llama-batch.h
mv src/llama-chat.cpp llama-chat.cpp
mv src/llama-chat.h llama-chat.h
mv src/llama-context.cpp llama-context.cpp
mv src/llama-context.h llama-context.h
mv src/llama-cparams.cpp llama-cparams.cpp
mv src/llama-cparams.h llama-cparams.h
mv src/llama-grammar.cpp llama-grammar.cpp
mv src/llama-grammar.h llama-grammar.h
mv src/llama-graph.cpp llama-graph.cpp
mv src/llama-graph.h llama-graph.h
mv src/llama-hparams.cpp llama-hparams.cpp
mv src/llama-hparams.h llama-hparams.h
mv src/llama-impl.cpp llama-impl.cpp
mv src/llama-impl.h llama-impl.h
mv src/llama-io.cpp llama-io.cpp
mv src/llama-io.h llama-io.h
mv src/llama-kv-cache.cpp llama-kv-cache.cpp
mv src/llama-kv-cache.h llama-kv-cache.h
mv src/llama-kv-cache-iswa.cpp llama-kv-cache-iswa.cpp
mv src/llama-kv-cache-iswa.h llama-kv-cache-iswa.h
mv src/llama-kv-cells.h llama-kv-cells.h
mv src/llama-memory.cpp llama-memory.cpp
mv src/llama-memory.h llama-memory.h
mv src/llama-memory-hybrid.cpp llama-memory-hybrid.cpp
mv src/llama-memory-hybrid.h llama-memory-hybrid.h
mv src/llama-memory-recurrent.cpp llama-memory-recurrent.cpp
mv src/llama-memory-recurrent.h llama-memory-recurrent.h
mv src/llama-mmap.cpp llama-mmap.cpp
mv src/llama-mmap.h llama-mmap.h
mv src/llama-model.cpp llama-model.cpp
mv src/llama-model.h llama-model.h
mv src/llama-model-loader.cpp llama-model-loader.cpp
mv src/llama-model-loader.h llama-model-loader.h
mv src/llama-model-saver.cpp llama-model-saver.cpp
mv src/llama-model-saver.h llama-model-saver.h
mv src/llama-quant.cpp llama-quant.cpp
mv src/llama-quant.h llama-quant.h
mv src/llama-sampling.cpp llama-sampling.cpp
mv src/llama-sampling.h llama-sampling.h
mv src/llama-vocab.cpp llama-vocab.cpp
mv src/llama-vocab.h llama-vocab.h
mv src/unicode-data.cpp unicode-data.cpp
mv src/unicode-data.h unicode-data.h
mv src/unicode.cpp unicode.cpp
mv src/unicode.h unicode.h

# Move include/llama*.h to root
mv include/llama.h llama.h
mv include/llama-cpp.h llama-cpp.h

# ============================================================================
# Section 3: GGML files (flatten from ggml/src/ and ggml/include/)
# ============================================================================

echo "Flattening ggml/ directory..."
mv ggml/src/ggml-alloc.c ggml-alloc.c
mv ggml/include/ggml-alloc.h ggml-alloc.h
mv ggml/src/ggml-backend.cpp ggml-backend.cpp
mv ggml/include/ggml-backend.h ggml-backend.h
mv ggml/src/ggml-backend-impl.h ggml-backend-impl.h
mv ggml/src/ggml-backend-reg.cpp ggml-backend-reg.cpp
mv ggml/src/ggml-common.h ggml-common.h
mv ggml/src/ggml-impl.h ggml-impl.h
mv ggml/src/ggml-opt.cpp ggml-opt.cpp
mv ggml/include/ggml-opt.h ggml-opt.h
mv ggml/src/ggml-quants.c ggml-quants.c
mv ggml/src/ggml-quants.h ggml-quants.h
mv ggml/src/ggml-threading.cpp ggml-threading.cpp
mv ggml/src/ggml-threading.h ggml-threading.h
mv ggml/src/ggml.c ggml.c
mv ggml/src/ggml.cpp ggml.cpp
mv ggml/include/ggml.h ggml.h
mv ggml/src/gguf.cpp gguf.cpp
mv ggml/include/gguf.h gguf.h

# ============================================================================
# Section 4: Tools/Applications
# Updated 2025: examples/ → tools/, examples/main/ → tools/cli/
# We KEEP the new naming (cli, mtmd, etc.)
# ============================================================================

echo "Flattening tools/ directory..."

# CLI tool (was examples/main/, now tools/cli/)
# KEEP new name "cli"
mkdir -p cli
mv tools/cli/cli.cpp cli/cli.cpp
mv tools/cli/README.md cli/README.md

# IMatrix tool
mkdir -p imatrix
mv tools/imatrix/imatrix.cpp imatrix/imatrix.cpp
mv tools/imatrix/README.md imatrix/README.md

# Benchmark tool
mkdir -p llama-bench
mv tools/llama-bench/llama-bench.cpp llama-bench/llama-bench.cpp
mv tools/llama-bench/README.md llama-bench/README.md

# Perplexity tool
mkdir -p perplexity
mv tools/perplexity/perplexity.cpp perplexity/perplexity.cpp
mv tools/perplexity/README.md perplexity/README.md

# Quantize tool
mkdir -p quantize
mv tools/quantize/quantize.cpp quantize/quantize.cpp
mv tools/quantize/README.md quantize/README.md

# Server (modularized in 2025)
mkdir -p server/public server/themes
mv tools/server/server.cpp server/server.cpp
mv tools/server/server-common.cpp server/server-common.cpp
mv tools/server/server-common.h server/server-common.h
mv tools/server/server-context.cpp server/server-context.cpp
mv tools/server/server-context.h server/server-context.h
mv tools/server/server-http.cpp server/server-http.cpp
mv tools/server/server-http.h server/server-http.h
mv tools/server/server-models.cpp server/server-models.cpp
mv tools/server/server-models.h server/server-models.h
mv tools/server/server-queue.cpp server/server-queue.cpp
mv tools/server/server-queue.h server/server-queue.h
mv tools/server/server-task.cpp server/server-task.cpp
mv tools/server/server-task.h server/server-task.h
mv tools/server/chat-llama2.sh server/chat-llama2.sh
mv tools/server/chat.mjs server/chat.mjs
mv tools/server/chat.sh server/chat.sh
mv tools/server/README.md server/README.md
mv tools/server/README-dev.md server/README-dev.md

# Server public files
mv tools/server/public/index.html.gz server/public/index.html.gz
mv tools/server/public/loading.html server/public/loading.html

# ============================================================================
# Section 5: Multimodal/mtmd tools
# Updated 2025: examples/llava/ → tools/mtmd/
# We KEEP the new name "mtmd" (multimodal tools directory)
# ============================================================================

echo "Flattening tools/mtmd/ (multimodal) directory..."
mkdir -p mtmd
mv tools/mtmd/clip.cpp mtmd/clip.cpp
mv tools/mtmd/clip.h mtmd/clip.h
mv tools/mtmd/clip-impl.h mtmd/clip-impl.h
mv tools/mtmd/clip-model.h mtmd/clip-model.h
mv tools/mtmd/clip-graph.h mtmd/clip-graph.h
mv tools/mtmd/deprecation-warning.cpp mtmd/deprecation-warning.cpp
mv tools/mtmd/mtmd-cli.cpp mtmd/mtmd-cli.cpp
mv tools/mtmd/mtmd.cpp mtmd/mtmd.cpp
mv tools/mtmd/mtmd.h mtmd/mtmd.h
mv tools/mtmd/mtmd-helper.cpp mtmd/mtmd-helper.cpp
mv tools/mtmd/mtmd-helper.h mtmd/mtmd-helper.h
mv tools/mtmd/mtmd-audio.cpp mtmd/mtmd-audio.cpp
mv tools/mtmd/mtmd-audio.h mtmd/mtmd-audio.h
mv tools/mtmd/README.md mtmd/README.md
mv tools/mtmd/requirements.txt mtmd/requirements.txt
mv tools/mtmd/tests.sh mtmd/tests.sh
mv tools/mtmd/test-1.jpeg mtmd/test-1.jpeg
mv tools/mtmd/test-2.mp3 mtmd/test-2.mp3

# Move model-specific files
mkdir -p mtmd/models
mv tools/mtmd/models/*.cpp mtmd/models/
mv tools/mtmd/models/*.h mtmd/models/

echo ""
echo "Renames completed."
echo "Note: New upstream naming preserved (cli, mtmd, etc.)"
echo "Review changes with: git status"
