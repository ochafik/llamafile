# Migration Log #001: Current Status and Problem

## Date
2025-12-28

## Context
We are upgrading llamafile's llama.cpp submodule from commit `8b3befc0e` (old) to upstream master `06705fdc` (new).

This is a prerequisite for integrating tool call (function calling) support - we want a clean upgraded base before adding the tool call feature.

## Current Status

### Completed
- ✅ Created git worktree at `/Users/ochafik/github/llamafile-upgrade`
- ✅ Updated llama.cpp submodule to upstream master (06705fdc)
- ✅ Identified major structural refactoring in llama.cpp

### In Progress
- ⏳ Analyzing patch system for compatibility

### Pending
- ⏸️ Update renames.sh for new file structure
- ⏸️ Update BUILD.mk files for new paths
- ⏸️ Fix patch system for new structure
- ⏸️ Test build
- ⏸️ Create tool-call branch from upgraded base
- ⏸️ Port tool call integration

## The Problem: Major llama.cpp Refactoring

The llama.cpp upgrade broke the build system because upstream made massive structural changes:

### 1. Directory Structure Changes

| Old Path | New Path |
|----------|----------|
| `examples/main/main.cpp` | `tools/cli/cli.cpp` |
| `examples/server/server.cpp` | `tools/server/` (8+ files) |
| `examples/llava/` | `tools/mtmd/models/` |
| `llama.cpp` (single file) | `src/llama-*.cpp` (24+ files) |
| `ggml.h`, `ggml.c` | `ggml/include/`, `ggml/src/` |
| `llama.h`, `llama.cpp` | `include/`, `src/` |

### 2. File Modularization

The single `llama.cpp` file was split into 24+ specialized files:
- `llama-adapter.cpp`
- `llama-arch.cpp`
- `llama-batch.cpp`
- `llama-chat.cpp`
- `llama-context.cpp`
- `llama-cparams.cpp`
- `llama-grammar.cpp`
- `llama-graph.cpp`
- `llama-hparams.cpp`
- `llama-impl.cpp`
- `llama-io.cpp`
- `llama-kv-cache.cpp`
- `llama-memory.cpp`
- `llama-mmap.cpp`
- `llama-model.cpp`
- `llama-quant.cpp`
- `llama-sampling.cpp`
- `llama-vocab.cpp`
- ... and more

### 3. Build System Change

- **Old**: Makefile-based build
- **New**: CMake-only build (Makefile now just shows error message)

### 4. API Changes

Tokenization API changed from:
```cpp
// Old
llama_tokenize(model, text, ...);

// New
llama_tokenize(vocab, text, ...);
```

## Impact on llamafile Build System

llamafile uses a custom build system based on:
- `llama.cpp.patches/renames.sh` - flattens llama.cpp directory structure
- `llama.cpp.patches/apply-patches.sh` - applies llamafile-specific patches
- `llama.cpp.patches/llamafile-files/BUILD.mk` - generated build configuration

These scripts are hardcoded for the old llama.cpp structure and need to be updated.

## Worktree Locations

- **Main repo**: `/Users/ochafik/github/llamafile`
- **Upgrade worktree**: `/Users/ochafik/github/llamafile-upgrade`
- **Tool call WIP worktree**: `/Users/ochafik/github/llamafile-chat` (chat-support branch)

## Next Steps

1. Search llama.cpp git history for refactoring commits/PRs
2. Analyze existing renames.sh to understand the transformation pattern
3. Update renames.sh for new structure
4. Generate new BUILD.mk files
5. Fix any patch incompatibilities
6. Test the build
