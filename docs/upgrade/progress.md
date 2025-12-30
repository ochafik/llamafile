# Build Progress - 2024-12-29

## Completed Changes

1. **GGML_CALL → LLAMAFILE_CALL**
   - Added `LLAMAFILE_CALL` macro to llamafile.h
   - Changed to expand to nothing (new llama.cpp doesn't use ms_abi)
   - Updated cuda.c and metal.c to use LLAMAFILE_CALL

2. **Metal API Migration**
   - Updated metal.c to match new llama.cpp Metal backend API
   - Removed obsolete functions:
     - `ggml_metal_link` → replaced with `ggml_backend_metal_reg`
     - `ggml_backend_metal_buffer_type` → removed
     - `ggml_backend_metal_buffer_from_ptr` → removed
     - `ggml_backend_metal_set_n_cb` → removed
     - `ggml_backend_metal_log_set_callback` → `ggml_backend_metal_set_abort_callback`
     - `ggml_backend_metal_get_device_memory_usage` → removed

3. **Minor Fixes**
   - Added `#include "llamafile/llamafile.h"` to schlep.c (for FLAG_warmup)
   - Commented out `llama_print_timings` call in chatbot_comm.cpp (removed in new llama.cpp)

## Current Issues

### llama_batch API Changes
The new `llama_batch` structure and `llama_batch_get_one` function have changed:
- `llama_batch_get_one()` now takes 2 arguments instead of 4
- Fields removed: `all_pos_0`, `all_pos_1`, `all_seq_id` → `n_seq_id`

### mtmd_tokenize API Change
- Signature changed: `const mtmd_bitmap **` instead of `mtmd_bitmap **`

### gpt_params Structure
- Need to include proper header for complete `gpt_params` definition

## Next Steps
1. Fix llama_batch API usage in chatbot files
2. Fix mtmd_tokenize calls
3. Update gpt_params includes

## Additional API Changes Found

### Sampling API Changes
The new llama.cpp has completely rewritten the sampling API:
- `llama_chat_msg` → `llama_chat_message`
- `llama_sampling_context` → `llama_sampler *` / `llama_sampler_context_t`
- `llama_sampling_sample` → `llama_sampler_sample`
- `llama_sampling_accept` → removed/changed
- `llama_sampling_free` → `llama_sampler_free`

### Vocab API Changes
- `llama_token_bos()` now takes `const struct llama_vocab *` instead of `llama_model *`
- `llama_should_add_bos_token()` now takes `const struct llama_vocab *` instead of `llama_model *`
- `llama_token_is_eog()` signature changed

### Server/CLI Headers
- `server.h` includes `llama.cpp/llama.h` → should be `llama.cpp/include/llama.h`

### Param Structure Changes
- `g_params.sparams` → might be `g_params.sampling`

These changes require significant updates to chatbot_repl.cpp and related files.

## Status: Build Progressing with Remaining Issues

### Fixed in chatbot_repl.cpp:
- `llama_chat_msg` → `llama_chat_message`
- `llama_sampling_context *` → `struct common_sampler *`
- `llama_sampling_init()` → `common_sampler_init()`
- `llama_sampling_sample()` → `common_sampler_sample()`
- `llama_sampling_accept()` → `common_sampler_accept()`
- `llama_sampling_free()` → `common_sampler_free()`
- `llama_token_bos(model)` → `llama_vocab_bos(vocab)`
- `llama_should_add_bos_token(model)` → `llama_vocab_get_add_bos(vocab)`
- `llama_token_is_eog(model, token)` → `llama_vocab_is_eog(vocab, token)`

### Remaining Issues in chatbot_main.cpp:
1. `g_params.sparams` → `g_params.sampling`
2. `gpt_params_parse` → `common_params_parse`
3. `llama_model_params_from_gpt_params` → new API needed
4. `llama_context_params_from_gpt_params` → new API needed
5. `g_params.model.c_str()` - model field structure changed
6. `llamafile_has()` - function signature might have changed
7. Various FLAG_* includes needed
8. Deprecated functions: `llama_n_ctx_train` → `llama_model_n_ctx_train`
9. Deprecated functions: `llama_new_context_with_model` → `llama_init_from_model`

### chatbot_eval.cpp:
- Still has one error about llama_n_ctx() being deprecated
