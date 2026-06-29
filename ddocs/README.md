# llamafile — agentic platform ddocs

Design + research + validated prototypes for turning llamafile into a self-contained **agentic platform**: a server (and CLI) that runs tool-enabled agents — including a multi-agent web UI, embedded offline Wikipedia, MCP support, browser automation, and a Claude-Code/opencode-style coding agent — all from one portable APE binary serving one local model with continuous batching.

Branch: `upgrade-2026` (off llamafile `main` / v0.10.x). Session: 2026-06-28/29.

## Documents
| # | Doc | What it covers | Headline |
|---|-----|----------------|----------|
| 01 | [mcp-wikipedia-design](01-mcp-wikipedia-design.md) | MCP support + embedded ZIM Wikipedia | One `server_tool` registry, two sources: **wiki inlined**, **MCP as a server-side host** bridging external servers. Native tool-calling replaces wikifile's dead-end pattern-detector. **P1/P2/P3 all validated.** |
| 02 | [multiagent-webui-design](02-multiagent-webui-design.md) | Orchestrator + ideators + researchers + verifiers in the web UI | **Agent-as-tool** on the existing agentic loop; no deadlock (slot held only while generating); recommend **`-np 8`** (+KV math). Mock harness validated; **live run on real Qwen works**. |
| 03 | [multiagent-best-practices-2026](03-multiagent-best-practices-2026.md) | June-2026 state of the art | Default 1 strong agent; **fan out for breadth/context-isolation only**; **one writer**; distilled payloads not transcripts; verify-before-commit; local-model reframe (parallel agents *are* the GPU batch; one model multiplies its own error rate). |
| 04 | [browser-tool-design](04-browser-tool-design.md) | Generic `browser_*` tool (web research) | In-process **CDP**; **`cpp-httplib` v0.48 ships a WS client → zero new deps**. **ATTACH-to-user-browser** + LAUNCH modes (validated vs real Chrome 149). 7 tools + Readability + security guardrails. |
| 05 | [agentic-coding-and-context](05-agentic-coding-and-context.md) | Long-context/compaction + coding-agent + CC/opencode drop-in | **Claude Code drop-in ~80% already built** (Anthropic `/v1/messages` API vendored, incl. a CC billing-header normalizer). Build+backend, don't vendor. Compaction: plan ~16–32K usable, trigger 70%, elide tool outputs first. |

## Architecture in one picture
```
/v1/chat/completions (+ /v1/messages = Anthropic API)  ── native tool-calling (jinja + autoparser)
        │ tool_calls
   agentic loop  (browser agenticStore today; + server-side /v1/agentic for CLI/non-browser)
        │ + agent-as-tool: orchestrator delegates to ideator/researcher/verifier sub-agents
   server_tool registry (GET/POST /tools)
     ├ built-in coding tools (read/write/edit/apply_diff/grep/glob/exec)   ← coding agent
     ├ wiki_search / wiki_get_article  → inlined pure-C ZIM reader          ← embedded Wikipedia
     ├ browser_* → in-process CDP (attach user browser, or launch)         ← web research
     └ MCP-bridged tools  → external MCP servers (stdio / HTTP-SSE)        ← extensibility
served by ONE llamafile + continuous batching (N slots) + (optional) bundled KV / QLoRA per role
```

## Prototypes (validated; drivers preserved in `prototypes/`)
| Proto | What it proves | Status |
|-------|----------------|--------|
| `prototypes/p1-zim-zimtest.c` + `p1-zim_search-fixed.c` | wikifile ZIM reader compiles under cosmocc 4.0.2; reads real v5/v6 ZIMs (zstd decompress, HTML→text, path lookup); **title-search bug found+fixed** (header title-ptr list is (namespace,title)-ordered) | ✅ P1 |
| (live test) | **Qwen3.6-35B-A3B** on `llamafile --server --jinja`: emits `search_wikipedia` tool_call → consumes result → grounded answer (full loop) | ✅ P2 |
| `prototypes/p3-mcp_client.cpp` + `p3-mcp_server_stub.py` | cosmoc++ MCP client `posix_spawn`s a server, full `initialize`/`tools/list`/`tools/call` | ✅ P3 |
| `prototypes/cdp_probe.py` | CDP LAUNCH (headless) + ATTACH (reuse running Chrome tab) navigate→extract, vs real Chrome 149 | ✅ |
| `prototypes/orchestrate.py` | agent-as-tool orchestration; `--mock` (offline) + live against the llamafile server | ✅ (mock + live) |

> Full ZIM reader (9 files) lives on the `wikifile` branch (`llamafile/zim/`); only the small prototype drivers are copied here. Test ZIMs: openzim zim-testing-suite (nons=v6, withns=v5).

## Cross-cutting: "bundle it in the APE" (the meta theme)
One portable binary can ship more than the model:
| Artifact | Feasibility | Notes |
|----------|-------------|-------|
| Model GGUF | ✅ (today) | existing zip layer (zip64) |
| ZIM Wikipedia | ✅ small inline / external `--zim` | `zim_open_fd` + zip; multi-GB → external path |
| **Precomputed agent-prompt KV** | ✅ worth it | instant cold-start + N× shared-prefix reuse across slots; strict fingerprint (model+quant+tokenizer+prompt+ctx/rope+**KV dtype**+state-version); golem `golem_kv_session` is prior art |
| **QLoRA role adapters** | ✅ per-slot LoRA *is* supported | role specialization from one base model. **Caveat:** different-adapter slots can't share a decode batch (serialize into per-adapter groups); a bundled KV is valid only for its exact adapter. Recommend prompt-only role steering first. |

## Key deployment findings / open items
- **CC drop-in is ~80% there** — `/v1/messages` already vendored; remaining = verify tool round-trip through it + beta-field tolerance. (doc 05)
- **`-np`/`--parallel` rejected by the combined binary's front-end** ("invalid argument: N") though `--server --help` lists it; auto-defaults to 4 slots. Bumping needs an arg-passthrough fix (or use the standalone `llama-server` target). (doc 01 §7)
- **SECURITY**: built-in file tools (`read_file`/`write_file`/`edit_file`/`apply_diff`) have **no cwd path-jail** — must add before shipping a coding agent. `exec_shell_command` has a 10s timeout. (doc 05)
- **Compaction & context**: plan for ~16–32K *usable* context (not advertised 256K); compaction invalidates KV from the first divergent token. (doc 05)
- x86 runtime check still owed for the Phase-4 IQ4_K AVX2 kernel (separate ik_llama initiative; see RESPAWN-2026-06.md on the `upgrade` branch).

## Suggested build order
1. Wire `wiki_search`/`wiki_get_article` `server_tool`s to the ported ZIM reader (P1) → first real embedded-wiki RAG.
2. Server-side `/v1/agentic` loop (≈200 lines) reusing existing tools → CLI coding agent + non-browser agentic.
3. MCP host (stdio) bridging external servers into the registry (P3 path).
4. `browser_*` tools via in-process CDP (doc 04), ATTACH mode + guardrails.
5. Multi-agent agent-as-tool in the web UI (doc 02); fix `-np` passthrough for `-np 8`.
6. Optional: bundled KV / per-role QLoRA.
7. Harden: file-tool path-jail; CC `/v1/messages` tool round-trip verification.

---

## Implementation status (live — 2026-06-29)
Doc set: 01 mcp-wikipedia · 02 multiagent-webui · 03 best-practices · 04 browser-tool · 05 agentic-coding/context · **06 server-side-tools-api** · **07 wikidata+graph-engines (+measured storage bench)** · **08 embeddable-vector-search**.

**SHIPPED (origin/upgrade-2026, llama.cpp UNTOUCHED — all llamafile-owned, no patches):**
- ✅ **`llamafile wikipedia search|get`** — fast (0.17s, no model load) offline Wikipedia CLI; works on real ~250k-article ZIM (fixes: `X/listing/titleOrdered/v1` title index + streaming-zstd content). Usable by humans/scripts/**CC-via-Bash**.
- ✅ **`llamafile mcp-server [--zim]`** — MCP server (stdio JSON-RPC) exposing `wiki_search`/`wiki_get_article` via a data-driven tool registry. `claude mcp add wikipedia -- llamafile mcp-server --zim wiki.zim`. The tool-exposure backbone (one handler, three surfaces — ddoc 06).
- ✅ **CC drop-in** — `/v1/messages` full multi-turn tool round-trip verified on Qwen3.6-35B-A3B.

**Verdicts (research closed):**
- **Wikidata store = SQLite + FTS5** (already vendored in cosmo w/ FTS5; measured: 0.02ms point / 0.10ms FTS @ scale; ~68GB@113M, −30–50% w/ ETL refresh). DuckDB's weakness was text-search, not lookups. Refresh the user's `~/github/ai/graphs/` ETL (newer dump + best-rank + unit-id) to build it.
- **Semantic search = `sqlite-vec`** (static in the same SQLite, brute-force ≤~1–5M) → **`usearch`** mmap-HNSW for real scale. `sqlite-vector` rejected (brute-force + Elastic License). Embed via llamafile `--embedding` (nomic-text-v1.5, reusing graphs/ stack).
- → **One SQLite file unifies exact + FTS5 + vector; one embedding path; zero new deps.**

**Build order progress:** 1 wiki ✅ → MCP server ✅ → **browser_* (CDP) [in progress]** → server-side agentic loop / server-side tools → multi-agent web UI → harden (file-tool path-jail) → [queued] video webcam-agent (`~/github/llama.cpp-video-ddocs/`, derisked, rides this stack).

---

## ✅ FINAL STATUS — initial vision COMPLETE (2026-06-29)
One portable `llamafile` (`origin/upgrade-2026`; llama.cpp pristine except the scripted patch layer). All runtime-verified:

| Capability | How | Status |
|---|---|---|
| **Offline Wikipedia** | `wiki_search`/`wiki_get_article`; full 49GB ZIM @0.01s resident; `llamafile wikipedia` CLI | ✅ |
| **Structured facts (Wikidata)** | `wikidata_search`/`_entity`/`_property` over SQLite+FTS5 (113M ent.); `llamafile wikidata` CLI | ✅ (full 68GB store building) |
| **Live web** | `browser_*` in-process CDP (LAUNCH + ATTACH-your-browser), SSRF-guarded | ✅ |
| **MCP server** | `llamafile mcp-server` exposes tools to CC/opencode/Cursor | ✅ |
| **MCP host** | `--mcp '<cmd>'` bridges external servers' tools into `/tools` + web UI | ✅ |
| **Claude Code drop-in** | Anthropic `/v1/messages` (full multi-turn tool round-trip) | ✅ |
| **Multi-agent** | `delegate_to_researcher/ideator/verifier` = sub-agents-as-tools (server-side loops); web UI/CLI/CC become orchestrators, no frontend change | ✅ |
| **Web UI** | embedded (fetch unblocked); agenticStore consumes `/tools` | ✅ serving |
| **Video webcam-agent** | `--webcam-agent` `/agent/*` SSE + embedded UI; watch→witness→act (fires MCP tool, e.g. send_email) + 30s clip/live relay | ✅ |
| **Search strategy** | FTS5 + LLM query-expansion (parallel OR'd terms); embeddings deferred | ✅ |
| **Security** | file-tool cwd path-jail (`--tools-root`); `-np 8` batching | ✅ |

**Design principle that held throughout:** every capability is a llamafile-owned module exposing one handler across surfaces (CLI / mcp-server / /tools / web UI / MCP), so **llama.cpp stayed pristine** (only `#ifdef LLAMAFILE_TUI` hooks + the scripted patch layer).

**Remaining = optional polish:** full Wikidata store finishing its background build · Wikidata ETL fidelity (best-rank/unit-normalize) + search tiebreak · Wikipedia FTS5 sidecar (full-text vs title) · multi-agent web-UI lane visualization (Svelte source build) · server-side auto-inject of the clip URL into `send_email` args · hybrid-model `ignore_frame` attention-only KV trim (needs a submodule patch) · GPU/Metal is the interactive-speed lever (video already runs on Metal).

---

## ✅✅ INTERACTIVE MULTI-AGENT RUNTIME COMPLETE (2026-06-29) — ddoc 09
The multi-agent *core* (delegate-tools) was upgraded into a full **in-process agent runtime** (requirements locked with the user). All llamafile-owned, llama.cpp pristine, `make check` green. 5 phases:

| Phase | Capability | How | Tests |
|---|---|---|---|
| 1 | **Mailbox mesh + async + traces** | `agent_runtime.{h,cpp}`: Agent+mailbox+Router+Scheduler(slot pool) + non-blocking `spawn_agent`/`send_message`/`await`/`list_agents`; SSE-mux by agent_id; `trace.jsonl`; **per-agent token accounting**; runaway guards | unit (router/mailbox/scheduler/await/guards) + mesh-demo (real trace tree, overlapping=concurrency) + model E2E 7/7 |
| — | **Crash fix (pre-existing SIGBUS)** | cosmo default thread stack ~80KiB → `/v1/chat`+tools grammar/autoparser overflows httplib workers → `server-http.cpp.patch` runs every handler on an **8MiB pthread** | repro→fixed; bisected (not path-jail) |
| — | **Agentic-flow EVAL** | `tests/eval/` E1–E4 ×N: success/N + **tokens across ALL agents** + multiplier | validated: E1/E3 2/2, **multi-agent = 2.51×** single-agent tokens |
| 2 | **Scheduling** | `wait`/`poll_until`/`schedule` as park+timer-wake (no slot held); `agent_predicate.h` | `agent_runtime_sched_test` 41 |
| 3 | **Code interpreter** | `code_run_js`/`code_render_html` via headless-CDP (reuse browser tool); in agent allowlists | verified vs Chrome 149 + `code_run_wrapper_test` |
| 4 | **Persisted sessions** | `agent_session.{h,cpp}` SessionManager; `/session/*` CRUD+pause/resume/stop; atomic snapshot (conversation=truth + re-prefill; KV fingerprint+fallback) | `agent_session_test` 38 + **pause→kill -9→restart→resume** correct |
| 5 | **Web UI** | `llamafile/agent_ui/agents.html` (ZIPOBJ `/zip`, served `/agents`): session manager + live agent-lane tree from SSE traces | served 200 + SSE mesh verified |

**New surface:** tools `spawn_agent/send_message/await/list_agents/wait/poll_until/schedule/code_run_js/code_render_html`; endpoints `/session/*` (+`/runtime/*` aliases), `/agents`. Runtime tests all in `make check`; `runtime_mesh_test.py` + `tests/eval/` are model-gated. Design + risks: ddoc 09.

**The full vision is now realized end-to-end:** offline Wikipedia · structured Wikidata facts · live web · MCP host+server · Claude-Code drop-in · **an interactive message-passing multi-agent runtime** (async, scheduling, code-interp, persisted sessions, live UI) · video witness-and-act webcam agent — one portable binary.
