# MTMD API Analysis: Old llava vs New mtmd

This document analyzes the API differences between the old llava API and the new mtmd (multimodal) API in llama.cpp, specifically for updating llamafile's server/slot.cpp.

## Executive Summary

The old llava API has been completely replaced by the new mtmd API. The new API is more generic (supporting both vision and audio), uses different data structures, and requires a different workflow for processing images.

## Key Data Structure Changes

### Old API: `llava_image_embed`

```c
struct llava_image_embed {
    float * embed;       // Embedding data
    int n_image_pos;     // Number of image positions/tokens
};
```

**Usage in slot.cpp:**
- Line 197-201: Created using `llava_image_embed_make_with_bytes()`
- Line 202-203: Checked for null to detect encoding failure
- Line 205: Used `n_image_pos` to get token count
- Line 207, 221, 229: Freed using `llava_image_embed_free()`
- Line 218: Accessed `embed` data directly

### New API: `mtmd_bitmap` and `mtmd_image_tokens`

```c
// Raw image/audio data
struct mtmd_bitmap {
    uint32_t nx;                    // Width (or n_samples for audio)
    uint32_t ny;                    // Height
    std::vector<unsigned char> data; // RGBRGBRGB... or float audio data
    std::string id;                 // Optional ID for KV cache tracking
    bool is_audio;                  // True if audio data
};

// Tokenized image representation
struct mtmd_image_tokens {
    uint32_t nx;                    // Tokens in X direction
    uint32_t ny;                    // Tokens in Y direction
    bool use_mrope_pos;             // Use M-RoPE position counting
    clip_image_f32_batch batch_f32; // Preprocessed image patches
    std::string id;                 // Optional ID for KV cache tracking
};
```

## Key API Function Changes

### 1. Context Initialization

**Old API:**
```c
struct clip_ctx * clip_ctx_;
clip_ctx_ = clip_model_load(FLAG_mmproj, FLAG_verbose);
```

**New API:**
```c
struct mtmd_context * mtmd_ctx_;
mtmd_context_params params = mtmd_context_params_default();
params.use_gpu = true;
params.n_threads = FLAG_threads_batch;
mtmd_ctx_ = mtmd_init_from_file(FLAG_mmproj, llama_get_model(ctx_), params);
```

**Changes:**
- `clip_ctx` -> `mtmd_context`
- `clip_model_load()` -> `mtmd_init_from_file()`
- New parameter structure required
- Must pass llama_model pointer for validation

### 2. Image Embedding Creation

**Old API:**
```c
llava_image_embed * image_embed =
    llava_image_embed_make_with_bytes(clip_ctx_,
                                      FLAG_threads_batch,
                                      (const unsigned char*)bytes.data(),
                                      bytes.size());
```

**New API (two-step process):**

Step 1: Create bitmap from bytes
```c
mtmd_bitmap * bitmap = mtmd_bitmap_init(nx, ny, data);
// Or use helper for auto-detection:
mtmd_bitmap * bitmap = mtmd_helper_bitmap_init_from_buf(mtmd_ctx_, buf, len);
```

Step 2: Tokenize with text prompt
```c
mtmd_input_text text = {
    .text = prompt,
    .add_special = true,
    .parse_special = true,
};
mtmd_input_chunks * chunks = mtmd_input_chunks_init();
int32_t res = mtmd_tokenize(mtmd_ctx_, chunks, &text, &bitmap, 1);
```

**Key Changes:**
- No direct `make_with_bytes` equivalent
- Must use `mtmd_tokenize()` which handles both text and images
- Images are marked in text with special marker (default: `<__media__>`)

### 3. Getting Image Token Count

**Old API:**
```c
int N = image_embed->n_image_pos;
```

**New API:**
```c
const mtmd_input_chunk * chunk = mtmd_input_chunks_get(chunks, idx);
size_t n_tokens = mtmd_input_chunk_get_n_tokens(chunk);
llama_pos n_pos = mtmd_input_chunk_get_n_pos(chunk); // For M-RoPE
```

**Changes:**
- `n_image_pos` -> `mtmd_input_chunk_get_n_tokens()`
- For M-RoPE models (Qwen2VL), `n_pos` may differ from `n_tokens`

### 4. Encoding Image

**Old API:**
```c
// Encoding was done automatically in llava_image_embed_make_with_bytes()
// No separate encode step needed
```

**New API:**
```c
// First encode to get embeddings
int32_t res = mtmd_encode_chunk(mtmd_ctx_, chunk);
if (res != 0) {
    // Handle error
}
float * embd = mtmd_get_output_embd(mtmd_ctx_);
```

**Changes:**
- Separate encoding step required
- Get embeddings via `mtmd_get_output_embd()`

### 5. Decoding/Evaluating Image

**Old API:**
```c
llama_decode(ctx_,
             { .n_tokens = n_eval,
               .embd = image_embed->embed + i * n_embd,
               .all_pos_0 = used,
               .all_pos_1 = 1 });
```

**New API (option 1 - manual):**
```c
int n_mmproj_embd = llama_n_embd(llama_get_model(ctx));
int n_pos_per_embd = mtmd_decode_use_mrope(mtmd_ctx_) ? 4 : 1;

// For normal models:
llama_batch batch = {
    .n_tokens = n_tokens,
    .embd = embd,
    .pos = positions,  // array of positions
    ...
};
llama_decode(ctx_, batch);

// For M-RoPE models: positions are 2D (nx, ny)
```

**New API (option 2 - using helper):**
```c
llama_pos new_n_past = n_past;
int32_t res = mtmd_helper_decode_image_chunk(
    mtmd_ctx_, ctx_, chunk, embd,
    n_past, 0, FLAG_batch, &new_n_past);
```

### 6. Memory Cleanup

**Old API:**
```c
llava_image_embed_free(image_embed);
clip_free(clip_ctx_);
```

**New API:**
```c
mtmd_input_chunks_free(chunks);
mtmd_bitmap_free(bitmap);
mtmd_free(mtmd_ctx_);
```

**Changes:**
- Separate cleanup for each data structure
- No `llava_image_embed_free()` equivalent

## Complete Migration Example

### Old Code (from slot.cpp:190-232)

```cpp
int Slot::eval_image(const std::string_view& bytes,
                     const ProgressCallback& progress)
{
    if (!ctx_)
        return uninitialized;
    if (!clip_ctx_)
        return no_vision_model;

    llava_image_embed* image_embed =
        llava_image_embed_make_with_bytes(clip_ctx_,
                                          FLAG_threads_batch,
                                          (const unsigned char*)bytes.data(),
                                          bytes.size());
    if (!image_embed)
        return encode_image_failed;

    int used = ctx_used();
    int N = image_embed->n_image_pos;
    if (used + N > ctx_size()) {
        llava_image_embed_free(image_embed);
        return out_of_context;
    }

    int processed = 0;
    int n_embd = llama_n_embd(llama_get_model(ctx_));
    for (int i = 0; i < N; i += FLAG_batch) {
        int n_eval = N - i;
        if (n_eval > FLAG_batch)
            n_eval = FLAG_batch;
        if (llama_decode(ctx_,
                         { .n_tokens = n_eval,
                           .embd = image_embed->embed + i * n_embd,
                           .all_pos_0 = used,
                           .all_pos_1 = 1 })) {
            llava_image_embed_free(image_embed);
            return decode_image_failed;
        }
        used += n_eval;
        processed += n_eval;
        if (progress)
            progress(processed, N);
    }
    llava_image_embed_free(image_embed);
    history_.emplace_back(new Image(bytes, N));
    return N;
}
```

### New Code Pattern

```cpp
int Slot::eval_image(const std::string_view& bytes,
                     const ProgressCallback& progress)
{
    if (!ctx_)
        return uninitialized;
    if (!mtmd_ctx_)
        return no_vision_model;

    // Step 1: Decode image to get dimensions
    int nx, ny, nc;
    unsigned char* data = stbi_load_from_memory(
        (const unsigned char*)bytes.data(), bytes.size(),
        &nx, &ny, &nc, 3);
    if (!data)
        return decode_image_failed;

    // Step 2: Create bitmap
    mtmd_bitmap* bitmap = mtmd_bitmap_init(nx, ny, data);
    stbi_image_free(data);

    // Step 3: Create input with marker
    mtmd_input_text input = {
        .text = "<__media__>",  // Will be replaced with image
        .add_special = false,
        .parse_special = true,
    };

    // Step 4: Tokenize
    mtmd_input_chunks* chunks = mtmd_input_chunks_init();
    int32_t res = mtmd_tokenize(mtmd_ctx_, chunks, &input, &bitmap, 1);
    mtmd_bitmap_free(bitmap);

    if (res != 0)
        return encode_image_failed;

    // Step 5: Check context size
    const mtmd_input_chunk* chunk = mtmd_input_chunks_get(chunks, 0);
    size_t N = mtmd_input_chunk_get_n_tokens(chunk);
    int used = ctx_used();
    if (used + (int)N > ctx_size()) {
        mtmd_input_chunks_free(chunks);
        return out_of_context;
    }

    // Step 6: Encode
    res = mtmd_encode_chunk(mtmd_ctx_, chunk);
    if (res != 0) {
        mtmd_input_chunks_free(chunks);
        return encode_image_failed;
    }

    // Step 7: Get embeddings
    float* embd = mtmd_get_output_embd(mtmd_ctx_);

    // Step 8: Decode (handle batching)
    int processed = 0;
    int n_embd = llama_n_embd(llama_get_model(ctx_));
    llama_pos n_past = used;

    for (int i = 0; i < (int)N; i += FLAG_batch) {
        int n_eval = (int)N - i;
        if (n_eval > FLAG_batch)
            n_eval = FLAG_batch;

        llama_pos new_n_past = n_past;
        int32_t decode_res = mtmd_helper_decode_image_chunk(
            mtmd_ctx_, ctx_, chunk, embd + i * n_embd,
            n_past, 0, n_eval, &new_n_past);

        if (decode_res != 0) {
            mtmd_input_chunks_free(chunks);
            return decode_image_failed;
        }

        n_past = new_n_past;
        processed += n_eval;
        if (progress)
            progress(processed, (int)N);
    }

    mtmd_input_chunks_free(chunks);
    history_.emplace_back(new Image(bytes, N));
    return N;
}
```

## Additional API Changes

### New Helper Functions

1. **mtmd_helper_bitmap_init_from_file()**
   - Load image/audio from file
   - Auto-detects format

2. **mtmd_helper_bitmap_init_from_buf()**
   - Load image/audio from memory buffer
   - Auto-detects format (image vs audio)

3. **mtmd_helper_eval_chunks()**
   - High-level function to evaluate all chunks
   - Handles both text and image/audio
   - Manages batching automatically

4. **mtmd_support_vision() / mtmd_support_audio()**
   - Check if model supports vision/audio

5. **mtmd_decode_use_non_causal()**
   - Check if non-causal attention needed (e.g., Gemma3)

6. **mtmd_decode_use_mrope()**
   - Check if M-RoPE positioning needed (e.g., Qwen2VL)

### Constants

**Old API:**
- `MTMD_DEFAULT_IMAGE_MARKER` = `"<__image__>"` (deprecated)

**New API:**
- `mtmd_default_marker()` returns `"<__media__>"`

## Clip Context Functions

The internal clip context is now encapsulated within mtmd_context. The following clip functions are now used internally by mtmd:

- `clip_init()` -> `mtmd_init_from_file()` (wrapper)
- `clip_free()` -> `mtmd_free()` (handles both vision and audio)
- `clip_n_mmproj_embd()` -> internal use
- `clip_image_preprocess()` -> internal use
- `clip_image_encode()` / `clip_image_batch_encode()` -> `mtmd_encode_chunk()`

## Summary of Required Changes in slot.cpp

1. **Include changes:**
   - Old: `#include "llama.cpp/mtmd/clip.h"`, `#include "llama.cpp/mtmd/mtmd.h"`
   - New: Same includes, but API is different

2. **Member variables:**
   - `clip_ctx_` -> `mtmd_ctx_`

3. **Constructor/Destructor:**
   - `clip_model_load()` -> `mtmd_init_from_file()`
   - `clip_free()` -> `mtmd_free()`

4. **eval_image() function:**
   - Complete rewrite needed
   - Use `mtmd_bitmap_init()` or `mtmd_helper_bitmap_init_from_buf()`
   - Use `mtmd_tokenize()` to create chunks
   - Use `mtmd_encode_chunk()` to encode
   - Use `mtmd_get_output_embd()` to get embeddings
   - Use `mtmd_helper_decode_image_chunk()` or manual llama_decode()

5. **eval_atoms() function:**
   - Needs similar changes for image processing
   - Consider using `mtmd_helper_eval_chunks()` for simpler implementation

## Testing Checklist

After migration, verify:
- [ ] Image loading works
- [ ] Context size checking works correctly
- [ ] Batching works correctly
- [ ] M-RoPE models (Qwen2VL) work if supported
- [ ] Non-causal attention models (Gemma3) work if supported
- [ ] Audio input works if supported
- [ ] KV cache tracking with bitmap IDs works
- [ ] Progress callbacks work correctly

## References

- New API headers: `/Users/ochafik/github/llamafile-upgrade/llama.cpp/tools/mtmd/mtmd.h`
- Helper API: `/Users/ochafik/github/llamafile-upgrade/llama.cpp/tools/mtmd/mtmd-helper.h`
- Internal clip API: `/Users/ochafik/github/llamafile-upgrade/llama.cpp/tools/mtmd/clip.h`
- Example usage: `/Users/ochafik/github/llamafile-upgrade/llama.cpp/tools/mtmd/mtmd-cli.cpp`
