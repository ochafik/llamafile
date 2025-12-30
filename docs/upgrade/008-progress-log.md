# Progress Log: llama.cpp Upgrade

## 2024-12-30 (Session 8 - Full-path includes)

### Commit: `593ae0bc` - feat: use full-path includes in llama.cpp for mkdeps compatibility

**Status:** Build in progress, fixing remaining compilation errors

**What was done:**
1. Applied `fix_includes.py` script to convert all #include statements in llama.cpp to use full paths
2. Modified 326 files in llama.cpp/ with full-path includes
3. Added GGML_MULTIPLATFORM support in llama.cpp/common files:
   - `common.cpp` - cache directory handling
   - `arg.cpp` - PATH_MAX include
   - `download.cpp` - PATH_MAX include
4. Created stub headers in llama.cpp/ root for mkdeps compatibility
5. Pushed branch to remote as backup

**Current compilation errors to fix:**

1. **Fixed:** `llama.cpp/ggml-cuda.h` path issues
   - Corrected to `llama.cpp/ggml/include/ggml-cuda.h`
   - Fixed in: `gpu.c`, `cuda.c`, `chatbot_main.cpp`

2. **Fixed:** `llama.cpp/ggml-metal.h` path issues
   - Corrected to `llama.cpp/ggml/include/ggml-metal.h`
   - Fixed in: `gpu.c`, `cuda.c`

3. **Fixed:** `llamafile/string.h` conflicts
   - Removed `string.h` and `string.cpp`

4. **MAJOR:** `simple.cpp` needs complete rewrite for new API
   - Old API: `gpt_params`, `llama_batch_get_one(tokens, n_eval, n_past, 0)`
   - Old API: `llama_tokenize(ctx, str, add_bos)` - now requires vocab*
   - Old API: `gpt_params_parse` - now `common_params_parse`
   - Old API: `llama_context_params_from_gpt_params` - removed
   - Old API: `llama_should_add_bos_token` - now `llama_add_bos_token`
   - Old API: `llama_sampling_sample/accept` - new sampler API
   - Old API: `llama_token_is_eog` - now `llama_vocab_is_eog`
   - Old API: `llama_token_to_piece(ctx)` - now requires vocab*
   - Old API: `llama_new_context_with_model` - now `llama_init_from_model`
   - Old API: `llama_load_model_from_file` - now `llama_model_load_from_file`

**Recommendation:**
The `simple.cpp` file needs to be rewritten to match the current llama.cpp examples.
This is a significant rewrite that should be based on `llama.cpp/examples/main/main.cpp`.

**Estimated remaining work:**
- Rewrite simple.cpp based on new llama.cpp examples
- Fix any remaining include path issues in other files
- Test the complete build
- Verify runtime functionality

---

## Previous sessions (see earlier .md files for details)

### Session 7: Execution Plan
- Created stub headers in llama.cpp/ root
- Added GGML_MULTIPLATFORM support

### Session 6: Include Path Script Issues
- Attempted full-path includes but hit mkdeps performance issues
- Reverted changes

### Sessions 1-5: Initial API migrations
- Converted llama_tokenize, llama_token_to_piece to vocab API
- Updated llama_kv_cache to llama_memory API
- Fixed llama_n_ctx_train, llama_n_embd, llama_token_bos
- Removed deprecated fields: embeddings_only, logits_all, seed, flash_attn
- Updated llama_batch API
- Renamed string.h to strlib.h to avoid shadowing
