# Wikifile: Offline Wikipedia for Llamafile

Wikifile adds ZIM file support to llamafile, enabling LLMs to search and retrieve Wikipedia articles offline. ZIM is the archive format used by [Kiwix](https://kiwix.org) for distributing compressed Wikipedia.

## Getting a ZIM File

Download a Wikipedia ZIM file from the [Kiwix library](https://library.kiwix.org/):

- **wikipedia_en_all_maxi** (~100GB) - Full English Wikipedia with images
- **wikipedia_en_all_nopic** (~50GB) - Full English Wikipedia without images
- **wikipedia_en_simple_all** (~1GB) - Simple English Wikipedia (good for testing)

## Usage

### Running with an External Model

Use the server binary with separate model and ZIM files:

```bash
# After 'make install', use 'llamafiler'
# Or use the build output directly: ./o//llamafile/server/main

# Basic: load model and ZIM archive
llamafiler -m model.gguf --zim wikipedia.zim

# With GPU acceleration and tool calling enabled
llamafiler -m model.gguf --zim wikipedia.zim --zim-tools -ngl 999

# Full example with all common options
llamafiler \
  -m mistral-7b-instruct-v0.2.Q4_K_M.gguf \
  --zim wikipedia_en_simple_all.zim \
  --zim-tools \
  -ngl 999 \
  -c 8192 \
  -l 0.0.0.0:8080
```

### Creating a Bundled Wikifile

Bundle the model, ZIM file, and default arguments into a single executable:

```bash
# 1. Create a .args file with default arguments
cat > .args << 'EOF'
-m
model.gguf
--zim
wikipedia.zim
--zim-tools
-ngl
999
--host
0.0.0.0
...
EOF

# 2. Copy the server binary (use build output or installed binary)
cp o//llamafile/server/main my-wikifile.llamafile
# Or after 'make install': cp /usr/local/bin/llamafiler my-wikifile.llamafile

# 3. Bundle everything together
zipalign -j0 \
  my-wikifile.llamafile \
  model.gguf \
  wikipedia.zim \
  .args

# 4. Run it!
./my-wikifile.llamafile
```

The `...` in the .args file specifies where user-provided CLI arguments are inserted.

### Tool Calling

When `--zim-tools` is enabled, the LLM can search and read Wikipedia articles during chat conversations using special patterns:

- `[SEARCH: query]` - Search for articles matching a query
- `[READ: A/Article_Title]` - Read a specific article's content

The system automatically injects instructions telling the LLM how to use these tools.

**Note:** Tool calling only works with non-streaming responses.

## Web UI

When a ZIM file is loaded, a Wikipedia button appears in the chat interface. Click it to open a search panel where you can:

- Search for articles by title
- Browse search results
- Click to insert article content into the chat

## API Endpoints

| Endpoint | Description |
|----------|-------------|
| `GET /zim/metadata` | Archive metadata (title, article count, etc.) |
| `GET /zim/search?q=query&limit=10` | Search articles by title |
| `GET /zim/suggest?q=prefix&limit=10` | Autocomplete suggestions |
| `GET /zim/article/A/Title` | Get article as plain text |
| `GET /zim/raw/A/Title` | Get raw article content (HTML) |
| `GET /zim/main` | Redirect to main page |

## Example Session

With `--zim-tools` enabled, the LLM can autonomously search Wikipedia:

```
User: What is the speed of light?
Assistant: Let me search Wikipedia for that information.
[SEARCH: speed of light]

[Wikipedia search results are injected here]

Based on the search results, I found the article. Let me read it.
[READ: A/Speed_of_light]

[Article content is injected here]

The speed of light in vacuum is exactly 299,792,458 metres per second...
```

## Building from Source

```bash
# Download toolchain and submodules
make setup

# Build using cosmocc's make (not system make)
.cosmocc/3.9.7/bin/make -j8

# Or just build the server
.cosmocc/3.9.7/bin/make -j8 o//llamafile/server/main
```

The ZIM library is built as part of the standard build process. The server binary is at `o//llamafile/server/main`.

## Architecture

- `llamafile/zim/` - Pure C library for reading ZIM files
  - `zim.c` - Core archive parsing
  - `zim_cluster.c` - Cluster decompression with LRU cache
  - `zim_search.c` - Title-based search
  - `zim_html.c` - HTML-to-text conversion
- `llamafile/server/zim.cpp` - HTTP endpoint handlers
- `llamafile/server/zim_tools.cpp` - Tool calling detection and execution

## Limitations

- Title-based search only (no full-text search)
- Streaming responses do not support tool calling (non-streaming only)
- Maximum 8 decompressed clusters cached in memory


## TODO

- [ ] Refactor C function calls: use `extern "C"` declarations in a separate header instead of `::` global namespace prefixes throughout the codebase
- [ ] Add full-text search support (currently title-based only)
- [ ] Support streaming responses with tool calling
- [ ] Add configuration for cluster cache size
