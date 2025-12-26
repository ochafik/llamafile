# Wikifile: Wikipedia-Augmented Llamafile

## Executive Summary

**Wikifile** extends llamafile to integrate ZIM file archives (Wikipedia/Kiwix format), enabling:
- Self-contained LLM + Wikipedia distribution in a single executable
- Search-augmented chat via tool calling
- CLI and web interface support for Wikipedia queries
- Offline knowledge retrieval without internet dependency

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        wikifile executable                       │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │   LLM Model │  │  ZIM Archive │  │   llamafile runtime    │  │
│  │   (GGUF)    │  │  (Wikipedia) │  │   (server + tools)     │  │
│  └─────────────┘  └─────────────┘  └─────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
   ┌─────────┐          ┌──────────┐         ┌──────────┐
   │   CLI   │          │   HTTP   │         │  Web UI  │
   │  (chat) │          │   API    │         │ (chat)   │
   └─────────┘          └──────────┘         └──────────┘
```

## Components

### 1. ZIM Reader Library (`llamafile/zim/`)

A minimal, cosmopolitan-compatible ZIM reader.

#### Core Files
```
llamafile/zim/
├── zim.h              # Public API
├── zim.c              # Core reader implementation
├── zim_header.h       # Header structure definitions
├── zim_cluster.c      # Cluster decompression
├── zim_search.c       # Title-based search (no Xapian)
└── BUILD.mk           # Build configuration
```

#### ZIM File Format Support

| Component | Description | Implementation |
|-----------|-------------|----------------|
| Header | 80-byte header with offsets | Direct parsing |
| MIME Types | Null-terminated string list | String parsing |
| Path Pointer List | 8-byte offsets to entries | Binary search |
| Title Pointer List | 4-byte indices (sorted by title) | Binary search |
| Directory Entries | Content or redirect entries | Struct parsing |
| Clusters | Compressed data containers | Decompression |
| Blobs | Individual article content | Offset extraction |

#### Compression Support

**Required additions to cosmocc/llamafile:**

| Format | ZIM ID | Status | Action Required |
|--------|--------|--------|-----------------|
| None | 1 | Ready | Use raw data |
| ZLIB/Deflate | 4 | Ready | Use existing `<third_party/zlib/zlib.h>` |
| Zstandard | 5 | **Missing** | Add zstd library |
| LZMA2/XZ | 4 | **Missing** | Add lzma library |

**Recommendation:** Bundle [zstd](https://github.com/facebook/zstd) single-file decoder (`zstddeclib.c`) - ~40KB source.

#### API Design

```c
// llamafile/zim/zim.h

typedef struct zim_archive zim_archive;
typedef struct zim_entry zim_entry;

// Archive operations
zim_archive* zim_open(const char* path);
zim_archive* zim_open_fd(int fd, size_t offset, size_t size);  // For embedded ZIM
void zim_close(zim_archive* archive);

// Metadata
const char* zim_get_name(zim_archive* archive);
uint32_t zim_get_entry_count(zim_archive* archive);
uint32_t zim_get_article_count(zim_archive* archive);

// Entry access
zim_entry* zim_get_entry_by_path(zim_archive* archive, const char* path);
zim_entry* zim_get_entry_by_index(zim_archive* archive, uint32_t index);
zim_entry* zim_get_main_entry(zim_archive* archive);

// Search (title-based, no Xapian)
typedef struct {
    uint32_t index;
    const char* title;
    const char* path;
    float score;  // Simple relevance score
} zim_search_result;

int zim_search_titles(zim_archive* archive, const char* query,
                      zim_search_result* results, int max_results);

// Content retrieval
const char* zim_entry_get_title(zim_entry* entry);
const char* zim_entry_get_path(zim_entry* entry);
const char* zim_entry_get_mimetype(zim_archive* archive, zim_entry* entry);
char* zim_entry_get_content(zim_archive* archive, zim_entry* entry, size_t* size);
void zim_free_content(char* content);

// HTML to text conversion (for LLM consumption)
char* zim_html_to_text(const char* html, size_t html_size, size_t* text_size);
```

### 2. Server Endpoints (`llamafile/server/`)

#### New Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/zim/search` | GET/POST | Search Wikipedia by query |
| `/zim/article/{path}` | GET | Get article content (HTML or text) |
| `/zim/suggest` | GET | Title autocomplete suggestions |
| `/zim/metadata` | GET | Archive metadata (name, entry count) |
| `/zim/raw/{path}` | GET | Raw content (images, CSS, etc.) |

#### Endpoint Specifications

```
GET /zim/search?q=<query>&limit=5&format=text
Response:
{
  "results": [
    {
      "title": "Albert Einstein",
      "path": "A/Albert_Einstein",
      "snippet": "German-born theoretical physicist...",
      "score": 0.95
    },
    ...
  ]
}

GET /zim/article/A/Albert_Einstein?format=text
Response:
{
  "title": "Albert Einstein",
  "path": "A/Albert_Einstein",
  "content": "Albert Einstein (14 March 1879 – 18 April 1955) was a German-born theoretical physicist...",
  "format": "text"
}

GET /zim/article/A/Albert_Einstein?format=html
Response: Raw HTML content with Content-Type: text/html
```

#### Implementation Pattern

Following llamafile's existing pattern in `client.cpp`:

```cpp
// In client.h
bool zim_search() __wur;
bool zim_article() __wur;
bool zim_suggest() __wur;
bool zim_metadata() __wur;

// In client.cpp dispatcher()
if (HasPrefix(p1, "zim/")) {
    std::string_view zim_path = p1.substr(4);
    if (zim_path == "search")
        return zim_search();
    if (zim_path == "suggest")
        return zim_suggest();
    if (zim_path == "metadata")
        return zim_metadata();
    if (HasPrefix(zim_path, "article/"))
        return zim_article();
    if (HasPrefix(zim_path, "raw/"))
        return zim_raw();
}
```

### 3. Tool Calling Integration

#### Tool Definitions

```json
{
  "tools": [
    {
      "type": "function",
      "function": {
        "name": "wikipedia_search",
        "description": "Search Wikipedia for articles matching a query. Returns titles and snippets.",
        "parameters": {
          "type": "object",
          "properties": {
            "query": {
              "type": "string",
              "description": "Search query (e.g., 'quantum physics', 'World War II')"
            },
            "limit": {
              "type": "integer",
              "default": 5,
              "description": "Maximum number of results to return"
            }
          },
          "required": ["query"]
        }
      }
    },
    {
      "type": "function",
      "function": {
        "name": "wikipedia_read",
        "description": "Read the full content of a Wikipedia article by its path.",
        "parameters": {
          "type": "object",
          "properties": {
            "path": {
              "type": "string",
              "description": "Article path from search results (e.g., 'A/Albert_Einstein')"
            },
            "max_length": {
              "type": "integer",
              "default": 4000,
              "description": "Maximum characters to return (for context window management)"
            }
          },
          "required": ["path"]
        }
      }
    }
  ]
}
```

#### Tool Calling Architecture

Since llamafile doesn't currently support OpenAI-style tool calling, we have two options:

**Option A: Automatic Tool Injection (Simpler)**

Modify the chat loop to:
1. Detect `[SEARCH: query]` or `[READ: path]` patterns in model output
2. Execute the tool and inject results
3. Continue generation

```cpp
// In v1_chat_completions.cpp generation loop
std::string piece = llamafile_token_to_piece(...);
if (auto cmd = parse_tool_command(accumulated_output + piece)) {
    // Execute tool
    std::string result = execute_wikipedia_tool(cmd);
    // Inject result and continue
    inject_tool_result(slot, result);
}
```

**Option B: Full OpenAI Tool Calling (More Work)**

Implement proper tool calling:
1. Accept `tools` parameter in `/v1/chat/completions`
2. Format tools into chat template
3. Parse structured tool calls from output
4. Return `tool_calls` in response
5. Accept `tool` role messages with results

**Recommendation:** Start with Option A for MVP, migrate to Option B later.

#### System Prompt for Tool Use

```
You are a helpful assistant with access to Wikipedia. When you need factual information:

1. Use [SEARCH: query] to find relevant Wikipedia articles
2. Use [READ: path] to read an article's content

Example:
User: What is the population of France?
Assistant: Let me search Wikipedia for that information.
[SEARCH: France population]
<system injects search results>
Based on the Wikipedia article, I can tell you that...
```

### 4. CLI Integration

#### New Flags

```
--zim <path>              Path to ZIM file (or embedded in executable)
--zim-search <query>      Search ZIM and print results (non-interactive)
--zim-read <path>         Read and print article content
--zim-tools               Enable Wikipedia tool calling in chat mode
```

#### CLI Tool Mode

```bash
# Search Wikipedia
$ llamafile --zim wikipedia.zim --zim-search "Albert Einstein"
1. Albert Einstein (A/Albert_Einstein) - German-born theoretical physicist...
2. Einstein field equations (A/Einstein_field_equations) - ...

# Read article
$ llamafile --zim wikipedia.zim --zim-read "A/Albert_Einstein" | head -50

# Chat with Wikipedia tools
$ llamafile -m model.gguf --zim wikipedia.zim --zim-tools
> What year did Einstein publish his theory of relativity?
[SEARCH: Einstein relativity publication year]
Based on Wikipedia: Einstein published his special theory of relativity in 1905...
```

### 5. Web UI Integration

#### UI Changes (`llamafile/server/www/`)

Add Wikipedia search widget:

```html
<!-- New search panel in index.html -->
<div id="wiki-panel" class="wiki-panel" style="display:none">
  <input type="text" id="wiki-search" placeholder="Search Wikipedia...">
  <div id="wiki-results"></div>
</div>
```

```javascript
// In chatbot.js
async function searchWikipedia(query) {
  const response = await fetch(`/zim/search?q=${encodeURIComponent(query)}&limit=5`);
  const data = await response.json();
  displayWikiResults(data.results);
}

// Auto-insert article reference into chat
function insertWikiArticle(path, title) {
  const input = document.getElementById('chat-input');
  input.value += `\n[Wikipedia: ${title}]\n`;
  // Optionally fetch and include snippet
}
```

### 6. Packaging & Distribution

#### Embedding ZIM in Executable

Like GGUF models, ZIM files can be appended to the executable:

```bash
# Create wikifile with embedded Wikipedia
cat llamafile model.gguf wikipedia.zim > wikifile.exe
zipalign -j0 wikifile.exe

# Or with external ZIM
./llamafile -m model.gguf --zim /path/to/wikipedia.zim
```

#### Size Considerations

| Content | Size | Notes |
|---------|------|-------|
| Wikipedia Mini (EN) | ~11 GB | All articles, compressed |
| Wikipedia Subset | ~1-2 GB | Top 100K articles |
| Simple Wikipedia | ~500 MB | Simplified English |
| Model (7B Q4) | ~4 GB | Typical small model |
| **Total** | ~15-17 GB | Complete offline package |

#### Custom ZIM Creation

For smaller distributions, create topic-specific ZIM files:

```bash
# Using zimwriterfs or zim-tools
zimwriterfs --welcome=index.html --favicon=icon.png \
  --language=en --title="Science Wikipedia" \
  --description="Science articles from Wikipedia" \
  ./science_articles/ science_wiki.zim
```

## Implementation Plan

### Phase 1: ZIM Reader Library (Week 1-2)

1. Implement header parsing
2. Implement directory entry parsing
3. Implement cluster decompression (zlib first)
4. Add zstd support (bundle decoder)
5. Implement path-based article retrieval
6. Add HTML-to-text conversion
7. Write unit tests

### Phase 2: Server Endpoints (Week 2-3)

1. Add `/zim/metadata` endpoint
2. Add `/zim/article/{path}` endpoint
3. Add `/zim/search` endpoint (title-based)
4. Add `/zim/raw/{path}` for static assets
5. Integrate with server startup (--zim flag)
6. Write integration tests

### Phase 3: Tool Calling (Week 3-4)

1. Implement pattern-based tool detection
2. Add tool execution in chat loop
3. Create system prompt templates
4. Add --zim-tools CLI flag
5. Test with various models

### Phase 4: Web UI (Week 4)

1. Add Wikipedia search widget
2. Integrate with chat interface
3. Add article preview panel
4. Style and polish

### Phase 5: Packaging (Week 5)

1. Support embedded ZIM in executable
2. Create example distributions
3. Write user documentation
4. Performance optimization

## Technical Challenges

### 1. Compression Library Dependencies

**Problem:** ZIM uses zstd (modern) and LZMA (older), neither in cosmocc.

**Solution:** Bundle single-file decoders:
- [zstd decompress-only](https://github.com/facebook/zstd/blob/dev/contrib/single_file_libs/zstddeclib.c) (~40KB)
- [minilzma](https://github.com/nicklockwood/minilzma) or similar for LZMA

### 2. Memory-Efficient Article Access

**Problem:** Large ZIM files (10GB+) can't be fully loaded.

**Solution:**
- Memory-map the ZIM file
- Cache decompressed clusters (LRU)
- Stream large articles in chunks

### 3. Search Without Xapian

**Problem:** Full-text search requires Xapian (complex dependency).

**Solution (MVP):**
- Title prefix matching (fast, simple)
- Substring search on titles
- Optional: Build simple inverted index at startup

**Solution (Future):**
- Port minimal Xapian subset
- Or use embedded SQLite FTS5

### 4. Tool Calling Model Compatibility

**Problem:** Not all models support tool calling patterns.

**Solution:**
- Use instruction-following models (Mistral, Llama-2-Chat, etc.)
- Provide clear examples in system prompt
- Use grammar constraints to enforce tool call format

## Configuration

### Runtime Flags

```
# ZIM-related flags
--zim <path>              Path to ZIM archive file
--zim-cache-size <MB>     Cluster cache size (default: 64)
--zim-tools               Enable Wikipedia tool calling
--zim-tools-auto          Auto-search for factual questions

# Combination example
./llamafile \
  -m mistral-7b.gguf \
  --zim wikipedia_en_mini.zim \
  --zim-tools \
  --host 0.0.0.0 \
  --port 8080
```

### Environment Variables

```bash
LLAMAFILE_ZIM_PATH=/path/to/wikipedia.zim
LLAMAFILE_ZIM_CACHE_MB=128
LLAMAFILE_ZIM_TOOLS=1
```

## Success Metrics

1. **Functionality:** Can search and retrieve Wikipedia articles
2. **Performance:** Article retrieval < 100ms, search < 500ms
3. **Accuracy:** Tool-augmented responses cite correct Wikipedia content
4. **Size:** Minimal code addition (~5K lines C/C++)
5. **Compatibility:** Works on all llamafile-supported platforms

## Open Questions

1. **Full-text search:** Worth the complexity for MVP?
2. **Image support:** Serve images from ZIM or text-only?
3. **Multiple ZIM files:** Support loading multiple archives?
4. **Caching strategy:** How aggressive should cluster caching be?
5. **Tool format:** Pattern-based or proper OpenAI tool calling?

## Implementation Status

### Completed Components

| Component | Status | Files |
|-----------|--------|-------|
| ZIM Header Parsing | Done | `llamafile/zim/zim.c` |
| Directory Entry Parsing | Done | `llamafile/zim/zim.c` |
| Path-based Entry Lookup | Done | `llamafile/zim/zim.c` |
| Cluster Decompression (zstd) | Done | `llamafile/zim/zim_cluster.c`, `zstddeclib.c` |
| Cluster Decompression (zlib) | Done | `llamafile/zim/zim_cluster.c` |
| LRU Cluster Cache | Done | `llamafile/zim/zim_cluster.c` |
| Title-based Search | Done | `llamafile/zim/zim_search.c` |
| HTML-to-Text Conversion | Done | `llamafile/zim/zim_html.c` |
| Server Endpoints | Done | `llamafile/server/zim.cpp` |
| CLI Flags (--zim, --zim-tools) | Done | `llamafile/flags.cpp` |
| Server ZIM Initialization | Done | `llamafile/server/prog.cpp` |
| Tool Calling (pattern-based) | Done | `llamafile/server/zim_tools.cpp` |
| Web UI Wikipedia Widget | Done | `llamafile/server/www/` |
| Unit Tests | Done | `llamafile/zim/zim_html_test.c`, `llamafile/server/zim_tools_test.cpp` |

### Files Created/Modified

#### New Files
- `llamafile/zim/zim.h` - Public C API for ZIM reader
- `llamafile/zim/zim_internal.h` - Internal structures and helpers
- `llamafile/zim/zim.c` - Core reader implementation
- `llamafile/zim/zim_cluster.c` - Cluster decompression with LRU cache
- `llamafile/zim/zim_search.c` - Title-based search implementation
- `llamafile/zim/zim_html.c` - HTML-to-text state machine converter
- `llamafile/zim/zstddeclib.c` - Facebook's single-file zstd decoder
- `llamafile/zim/BUILD.mk` - Build configuration
- `llamafile/server/zim.h` - Server ZIM lifecycle functions
- `llamafile/server/zim.cpp` - Server endpoint implementations
- `llamafile/server/zim_tools.h` - Tool calling utilities
- `llamafile/server/zim_tools.cpp` - Pattern detection and tool execution
- `llamafile/server/www/wiki.svg` - Wikipedia icon

#### Modified Files
- `llamafile/llamafile.h` - Added FLAG_zim and FLAG_zim_tools declarations
- `llamafile/flags.cpp` - Added --zim and --zim-tools flag parsing
- `llamafile/server/client.h` - Added ZIM endpoint declarations
- `llamafile/server/client.cpp` - Added ZIM dispatcher routes
- `llamafile/server/prog.cpp` - Added ZIM initialization at startup
- `llamafile/server/v1_chat_completions.cpp` - Added tool calling integration
- `llamafile/server/BUILD.mk` - Added ZIM library dependency
- `llamafile/server/www/index.html` - Added Wikipedia button and panel
- `llamafile/server/www/chatbot.css` - Added Wikipedia panel styles
- `llamafile/server/www/chatbot.js` - Added Wikipedia search functionality

### Usage

```bash
# Start server with ZIM support
./llamafile -m model.gguf --zim wikipedia.zim

# Enable tool calling in chat
./llamafile -m model.gguf --zim wikipedia.zim --zim-tools

# Test endpoints
curl http://localhost:8080/zim/metadata
curl "http://localhost:8080/zim/search?q=Einstein"
curl "http://localhost:8080/zim/article/A/Albert_Einstein?format=text"
```

### Known Limitations

1. **LZMA compression not supported** - Only zstd and zlib decompression implemented
2. **No full-text search** - Title-based search only (no Xapian integration)
3. **Non-streaming tool calling** - Tool results only work for non-streaming responses
4. **Single ZIM file** - Can only load one ZIM archive at a time

## References

- [ZIM File Format Specification](https://wiki.openzim.org/wiki/ZIM_File_Format)
- [libzim Reference Implementation](https://github.com/openzim/libzim)
- [Kiwix Project](https://www.kiwix.org/)
- [llamafile Documentation](https://github.com/Mozilla-Ocho/llamafile)
- [Cosmopolitan Libc](https://github.com/jart/cosmopolitan)
