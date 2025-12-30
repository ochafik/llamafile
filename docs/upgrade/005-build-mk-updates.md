# Migration Log #005: BUILD.mk Updates and API Compatibility Issue

## Date
2025-12-28

## Summary
Updated BUILD.mk files to use new upstream naming (cli, mtmd). Identified significant API incompatibility that needs to be addressed.

## Changes Made

### 1. Directory Renames in llamafile-files

| Old Path | New Path |
|----------|----------|
| `llamafile-files/main/` | `llamafile-files/cli/` |
| `llamafile-files/llava/` | `llamafile-files/mtmd/` |

### 2. File Renames

| Old File | New File |
|----------|----------|
| `cli/main.1` | `cli/cli.1` |
| `cli/main.1.asc` | `cli/cli.1.asc` |
| `mtmd/llava-quantize.1` | `mtmd/mtmd-quantize.1` |
| `mtmd/llava-quantize.1.asc` | `mtmd/mtmd-quantize.1.asc` |
| `mtmd/llava-quantize.cpp` | `mtmd/mtmd-quantize.cpp` |

### 3. BUILD.mk File Updates

#### cli/BUILD.mk
- Changed package name: `LLAMA_CPP_MAIN` → `LLAMA_CPP_CLI`
- Updated all paths: `llama.cpp/main/` → `llama.cpp/cli/`
- Updated dependency: `llama.cpp/llava/llava.a` → `llama.cpp/mtmd/mtmd.a`
- Updated target: `o/$(MODE)/llama.cpp/main/main` → `o/$(MODE)/llama.cpp/cli/cli`

#### mtmd/BUILD.mk
- Changed package name: `LLAMA_CPP_LLAVA` → `LLAMA_CPP_MTMD`
- Updated all paths: `llama.cpp/llava/` → `llama.cpp/mtmd/`
- Updated target: `llava.a` → `mtmd.a`
- Updated target: `llava-quantize` → `mtmd-quantize`

#### llamafile-files/BUILD.mk
- Updated includes:
  - `llama.cpp/llava/BUILD.mk` → `llama.cpp/mtmd/BUILD.mk`
  - `llama.cpp/main/BUILD.mk` → `llama.cpp/cli/BUILD.mk`
- Updated .PHONY targets:
  - `llama.cpp/main` → `llama.cpp/cli`
  - `llama.cpp/llava` → `llama.cpp/mtmd`

#### llamafile/server/BUILD.mk
- Updated dependency: `llama.cpp/llava/llava.a` → `llama.cpp/mtmd/mtmd.a`

### 4. Source Code Updates

#### llamafile/server/slot.cpp
- Updated includes:
  - `llama.cpp/llava/clip.h` → `llama.cpp/mtmd/clip.h`
  - `llama.cpp/llava/llava.h` → `llama.cpp/mtmd/mtmd.h`

## API Incompatibility Issue

### Problem
The new llama.cpp mtmd API is significantly different from the old llava API:

| Old API (llava) | New API (mtmd) |
|-----------------|----------------|
| `llava_image_embed` | `mtmd_bitmap` / `mtmd_image_tokens` |
| `llava_image_embed_make_with_bytes()` | `mtmd_image_embed_make_with_bytes()` (?) |
| `llava_image_embed_free()` | `mtmd_image_embed_free()` (?) |
| `llava_ctx` | `mtmd_context` |

### Impact
The `llamafile/server/slot.cpp` file uses `llava_image_embed_*` functions that may not exist in the same form in the new API. This will cause compilation errors.

### Required Changes
The slot.cpp code will need to be updated to use the new mtmd API. This may require:
1. Understanding the new mtmd API structure
2. Rewriting the image embedding logic
3. Potentially adding compatibility wrappers

## Status

**Completed:**
- ✅ Updated all BUILD.mk files to use new naming
- ✅ Updated include paths in slot.cpp
- ✅ Renamed directories and files

**Pending:**
- ⏸️ Fix mtmd API compatibility in slot.cpp
- ⏸️ Test the build
- ⏸️ Update any other files that reference the old API

## Next Steps

1. Investigate the new mtmd API in detail
2. Update slot.cpp to use the new API
3. Test the build
4. Run tests to verify functionality
