# llamafile: MCP support + embedded Wikipedia — design (2026-06-29)

> Design synthesized from 4 grounded research passes (server tool-calling, the `wikifile` prior art, MCP-under-cosmopolitan, ZIM reader/embedding). Status: **design locked, prototyping next.** Validation results appended in §7 as prototypes land.

## 1. Goals
- **MCP support in llamafile** — the *server* (not just the browser UI) can use MCP tools.
- **Embedded Wikipedia** — the llamafile server can answer from an offline ZIM Wikipedia, queryable by the model as a tool.
- Single portable APE binary; cosmocc; only vendored deps.

## 2. The pivotal finding — the integration llamafile lacked is now native
The `wikifile` branch (Dec 2025, "Broken") already built a **complete, reusable pure-C ZIM reader** (`llamafile/zim/`) + working HTTP endpoints. It failed *only* at the model↔tool layer: the old server **rejected the `tools` field**, streamed only `delta.content`, and fell back to a `[SEARCH:]`/`[READ:]` regex pattern-detector with fake `role:user` tool results — a dead end.

Modern llamafile (v0.10.x, post-reload) fixes every one of those at the root:
- **Native OpenAI tool-calling**: jinja renders tool defs into the prompt; the **autoparser** (`chat-peg-parser`) extracts `tool_calls`; responses carry `finish_reason: "tool_calls"`. (`server-common.cpp`, `server-task.cpp`)
- **A `server_tool` framework** (`llama.cpp/tools/server/server-tools.cpp`, gated by `--tools`/`--agent`): a registry of built-in tools (`read_file`, `grep_search`, `exec_shell_command`, …) exposed via `GET/POST /tools`.
- **A browser agentic loop** (`tools/ui` `agenticStore`): gets `tool_calls` → dispatches BUILTIN→`POST /tools`, MCP→the MCP server directly → appends `role:tool` results → re-POSTs until done.

So the rebuild is low-risk: **reuse the ZIM reader, drop the pattern-detector, expose Wikipedia through the native `server_tool` registry.**

## 3. Architecture — one registry, two tool sources
The `server_tool` registry IS the "inlined tool" mechanism. For shipped-together tools, "MCP" is just vocabulary (per the MCP-cosmo research) — the only thing the model sees is the JSON schema in `tools`. So:

```
                 /v1/chat/completions (native tool-calling; UNCHANGED, pure OpenAI)
                          │ tool_calls
        ┌─────────────────┴───────────────────┐
        │   agentic loop (browser today;        │
        │   llamafile-owned /v1/agentic/chat    │
        │   for non-browser, later)             │
        └─────────────────┬───────────────────┘
                          │ execute
                  ┌────────┴─────────┐
        server_tool registry (GET/POST /tools)
          ├── built-in (existing): read_file, grep, …
          ├── WIKI (new, INLINED): wiki_search, wiki_get_article  ──▶ llamafile/zim/ reader
          └── MCP-bridged (new): each external MCP server's tools registered here,
                                  execution forwarded over stdio/HTTP to that server
```

- **Wikipedia = inlined** `server_tool`s backed by the in-process ZIM reader. No subprocess.
- **MCP = a server-side host** that connects to user-configured MCP servers and **registers their tools into the same `server_tool` registry**, forwarding execution. The existing browser loop + `/tools` drive both uniformly.
- All new code in **`llamafile/`-owned files** — avoid patching the heavily-patched upstream `tools/server/` (keeps future llama.cpp syncs cheap). One tiny `server.cpp` registration call.

## 4. Wikipedia subsystem
- **Reader**: port `llamafile/zim/` from `wikifile` verbatim (pure C; zstd via vendored `zstddeclib.c`; zlib via cosmocc; LRU cluster cache; title-prefix binary search; HTML→text). Fix the documented cosmocc patterns: `extern "C"` (not `::` prefix), and the `#include "zstddeclib.c"` SRCS-exclusion in `BUILD.mk` (already done in wikifile). **Add LZMA** (XZ Embedded, ~6 files) only if we must support pre-2021 ZIMs — defer; modern Wikipedia ZIMs are all zstd.
- **Tools** (new `server_tool`s):
  - `wiki_search(query, limit)` → list of `{title, snippet}` via title binary-search + scoring.
  - `wiki_get_article(title)` → plain-text article (HTML→text) for RAG context.
- **Full-text search**: title binary-search baseline (the ZIM's Xapian 'X' index needs libxapian — heavy/GPL, rejected). Sufficient for RAG (model proposes titles; can retry). BM25-over-titles is an optional later enhancement.
- **Delivery**: `--zim /path/file.zim` external (primary). Optional in-APE embed for small ZIMs via the scaffolded `zim_open_fd(fd, offset, size)` + the existing **zip64-capable** zip layer (`zipalign` append, like the model). Use `pread`-per-cluster (NOT full mmap of a multi-GB file). Add a thin `llamafile_open_zim()` returning `{fd, base_offset}` (~50-line variant of `llamafile_open_zip`).
- Keep the 6 `/zim/*` HTTP endpoints (reusable) for direct UI browsing; the model path goes through the tools.

## 5. MCP subsystem
- **Client** (`llamafile/mcp_client.{h,cpp}`, new): JSON-RPC 2.0 over **newline-delimited** stdio (the MCP framing, not LSP Content-Length). Handshake: `initialize` → `notifications/initialized` → `tools/list` → `tools/call`. ~150 lines on vendored `nlohmann/json` 3.12.
- **Transports**: **stdio subprocess primary** (`posix_spawn` + `pipe2` + file-actions — all present in cosmocc 4.0.2 and already used cross-platform incl. Windows in `gpu_backend.c`); **HTTP/SSE secondary** via vendored `cpp-httplib` `SSEClient` (for remote/persistent servers). Do NOT implement the deprecated 2024 HTTP+SSE two-endpoint transport.
- **Bridge**: a `mcp_host` connects configured servers (e.g. `--mcp 'cmd args'` or a config file), `tools/list`s each, and registers every tool as a `server_tool` whose `execute()` forwards a `tools/call`.
- **Agentic loop for non-browser**: a llamafile-owned `/v1/agentic/chat` (or hook `ApiBackend::complete()` for TUI) that runs the tool loop server-side so curl/SDK clients (not just the web UI) get tool use. Phase it after the browser-driven path works.

## 6. Why not "wikipedia as an MCP subprocess"
Considered (the user floated it). Rejected as the *default* for the embedded case: it adds process lifecycle, IPC serialization, a second bundled binary, and Windows subprocess edge cases — all to wrap a function call that ships in the same binary. The inlined `server_tool` gives identical model-facing UX (same JSON schema) with none of that. The stdio-subprocess path is retained for *third-party* MCP servers, where isolation/language-independence/reuse actually pay for themselves.

## 7. Prototype / validation plan (load-bearing assumptions)
- **P1 — ZIM reader builds + reads a real ZIM under cosmocc 4.0.2.** Port `llamafile/zim/`, build its tests + a tiny CLI, open a small modern (zstd) ZIM: title lookup + article fetch + HTML→text. *De-risks: reader still compiles on the new toolchain; zstddeclib compiles.*
- **P2 — native model→tool round-trip.** Register a trivial `server_tool`, run the server with `--tools` + a tool-capable model, confirm the model emits a `tool_call`, the autoparser yields `finish_reason:tool_calls`, and `/tools` executes it. *De-risks THE thing wikifile failed at.*
- **P3 — MCP stdio client.** Minimal client spawns a trivial MCP server (tiny script), does `initialize`/`tools/list`/`tools/call`. *De-risks the cosmo subprocess+JSON-RPC path (already half-proven by `gpu_backend.c`).*

### P1 — ZIM reader under cosmocc — ✅ VALIDATED (2026-06-29)
Ported `wikifile`'s `llamafile/zim/` (9 files) into a standalone cosmocc harness; tested against two real ZIMs from the openzim testing-suite (v6/nons 41KB, v5/withns 79KB).
- ✅ **Compiles clean under cosmocc 4.0.2** (incl. the `#include "zstddeclib.c"` zstd decoder) → 918KB APE.
- ✅ Opens both v5 and v6; reads header/metadata; iterates entries; **decompresses zstd clusters**; **HTML→text**; **path/URL lookup** (`C/main.html` found).
- ❌→✅ **Found a real bug**: the header title-pointer list is ordered by **(namespace, title)**, not globally by title (dumped: `title_ptrs[0]='Test ZIM file'(C), [1]='favicon.png'(C), [2]='Counter'(M)…`). The wikifile binary search compared title-only → never matched. **Fixed** (content-namespace-aware search): `search "Test"→"Test ZIM file"`, `"favicon"→favicon.png` + article fetch all work.
- **Implication**: the Wikipedia subsystem design is sound — reader reusable, **title-search-without-Xapian proven viable**. The #1 implementation task is the title-search fix: binary-search within the content-namespace sub-range, or (more robust for real Wikipedia) use the `X/listing/titleOrdered/v1` article index. The wikifile reader needs: this search fix + `extern "C"` + the embed wiring (`zim_open_fd`); decompress/html/path-lookup are reusable as-is.
- Prototype artifacts: `scratchpad/zimproto/` (harness `zimtest.c`, the patched `zim_search.c`, test ZIMs).

### P3 — MCP stdio client under cosmocc — ✅ VALIDATED (2026-06-29)
Built a minimal MCP client (`scratchpad/mcpproto/mcp_client.cpp`, ~110 lines) with **cosmoc++** against the vendored `nlohmann/json` 3.12 (1.5M APE). It `posix_spawn`s a stub MCP server (`mcp_server_stub.py`), opens bidirectional pipes (`pipe2` + `posix_spawn_file_actions_adddup2`), and runs the real newline-JSON-RPC handshake:
- `initialize` → `server=stub-mcp protocol=2025-06-18`; `notifications/initialized` (notify, no reply); `tools/list` → `echo`; `tools/call echo` → `"echo: hello from cosmocc MCP client"`; clean server exit 0.
- Confirms: cosmocc subprocess+pipe IPC + newline-framed JSON-RPC with nlohmann all work → the MCP stdio transport is viable. (HTTP/SSE transport still relies on `cpp-httplib SSEClient`, untested but lower-risk.)

### P2 — native model→tool round-trip — IN PROGRESS
Downloading `unsloth/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf` (UD/Unsloth-Dynamic, 22GB; arch `qwen35moe`, supported by the vendored llama.cpp). Plan: run `llamafile --server --jinja` + POST `/v1/chat/completions` with a `search_wikipedia` tool def + a triggering prompt; confirm `finish_reason:"tool_calls"` + a parsed `tool_call` (proves the native tool-calling the `wikifile` effort lacked, on a real tool-capable model).

## 8. Phased implementation (after prototypes pass)
1. Port `llamafile/zim/` + `--zim` + `/zim/*` endpoints (reader only). Gate: open a ZIM, search, fetch.
2. `wiki_search` / `wiki_get_article` as `server_tool`s. Gate: model RAG-answers from the ZIM via native tool-calling.
3. In-APE embedding via `zim_open_fd` + zipalign (small ZIMs). Gate: a self-contained wiki-llamafile.
4. `mcp_client` (stdio) + `mcp_host` bridge → MCP tools as `server_tool`s. Gate: attach a real third-party MCP server, model calls it.
5. HTTP/SSE transport; `/v1/agentic/chat` for non-browser. Gate: curl-driven tool loop.

## 9. Top risks
- **R1** zstddeclib on cosmocc 4.0.2 — *low* (compiled in wikifile; re-verify P1).
- **R2** native tool-calling quality depends on the model's chat template (jinja tool support) — verify with a known tool-calling model in P2; document supported models.
- **R3** in-APE embed of multi-GB ZIM — sidestepped by defaulting to external `--zim`; embed only small ZIMs.
- **R4** MCP subprocess Windows edge cases (pipe poll) — mitigate with a blocking reader thread; HTTP fallback.
