# llamafile: server-side / provider-run tools over `/v1/messages` + `/v1/chat/completions` — design (2026-06-29)

> **Question (user-flagged "maybe a bad idea, just explore"):** should llamafile expose its built-in `server_tool`s as **provider-run / server-side tools** — i.e. llamafile runs the tool loop *under the hood* and returns results inline, so thin clients (curl, basic UIs, our own sub-agents) don't implement a tool loop, mirroring Anthropic's hosted `web_search`?
>
> **Convention:** **[E]** = EVIDENCE (repo file:line read on 2026-06-29, or a cited URL accessed 2026-06-29). **[I]** = INFERENCE over evidence. Repo paths are against the tree at `/Users/ochafik/github/llamafile-upgrade2026`.
>
> **Cross-refs:** registry architecture = `01-mcp-wikipedia-design.md`; the server-side agentic loop (build step 2) = `05-agentic-coding-and-context.md` §C6/§C9; build order = `README.md`.

---

## 0. Verdict (read this first)

**Worth doing — but as a thin, opt-in v1, built strictly *on top of* the already-planned step-2 server-side loop, never as a replacement for the existing client-tool path.** [I]

- **DO:** add a per-tool **`server_run` flag** to llamafile's `server_tool` registry and one **opt-in switch** (`--server-side-tools[=wiki,browser]`, default **off**). When on, the step-2 loop auto-executes *only llamafile-owned, server_run-flagged* tools inline and continues the turn; **every tool the client declared in the request passes through unchanged** as `tool_use` / `stop_reason:"tool_use"`. This is exactly Anthropic's "mix server + client tools in one turn" rule, which we already half-implement. [E, see §1.4]
- **DON'T (v1):** faithfully emulate Anthropic's `server_tool_use` + `*_tool_result` streaming content blocks with `srvtoolu_` ids, `pause_turn`, encrypted-citation round-tripping, and `usage.server_tool_use` accounting. That is a large, brittle surface for near-zero benefit to thin clients. v1 = **run the loop, return the final assistant message**, optionally surfacing what ran as ordinary visible `text`/`thinking` blocks. Promote to faithful blocks later *only if* a real client needs them.
- **WHERE in the build order:** **after step 2** (the server-side loop) and after step 1 (wiki tools) / step 4 (browser tools) exist to be worth auto-running. It is a ~1-flag + ~1-bool feature layered on the loop, not a new subsystem. Slot it as **step 2.5 / a sub-item of step 2**, gated behind the loop landing.
- **The hard constraint that makes it safe:** Claude Code and every other agentic client run *their own* loop and tools. Auto-running *their* tools would silently break them. The design is safe **only because llamafile's registry tools live in a different namespace than the client's request `tools[]`**, and we auto-run only the former. [I]
- **The organizing frame (one handler, three exposure surfaces, §7):** every tool is one `server_tool` (`get_definition()`+`invoke()`), exposed three ways by *who drives the loop* — (1) **internal registry** `GET/POST /tools` (llamafile's own loop/UI); (2) **server-side tools** (this doc — *llamafile* runs the loop for thin clients); (3) **MCP server** `--mcp-server` (the *external* client — CC/opencode/Cursor — runs the loop, llamafile just exposes its tools). Server-side tools and MCP-server are the two halves of "give external code llamafile's embedded tools," split by loop ownership. Server-side tools is **not** the right surface for loop-running clients — **MCP server is** (§7). [I]

---

## 1. EVIDENCE — Anthropic server tools, exact 2026 wire protocol

All from `platform.claude.com/docs/en/agents-and-tools/tool-use/{overview,server-tools,web-search-tool}`, accessed 2026-06-29. [E]

### 1.1 What is actually "server-run" vs "client-run"
Anthropic splits tools by **where the code executes**: [E, overview]

| Tool | Declared as | Executes where | llamafile-relevant? |
|---|---|---|---|
| `web_search` | `{"type":"web_search_20250305"\|..._20260209\|..._20260318,"name":"web_search"}` | **Anthropic infra (server)** | ✔ analog = our `wiki_search`/`browser_*` |
| `web_fetch` | `{"type":"web_fetch_20250910",...}` | **server** | ✔ analog = `wiki_get_article`/`browser` fetch |
| `code_execution` | `{"type":"code_execution_20260120",...}` | **server** (sandbox container) | partial = our `exec_shell_command` (but local!) |
| `tool_search` | `{"type":"tool_search_...",...}` | **server** | maybe (MCP tool discovery) |
| `advisor` | server tool | **server** | n/a |
| MCP connector | `mcp_tool_use` blocks | **server** (Anthropic connects out) | ✔ our MCP host is the local analog |
| **`bash`** | `{"type":"bash_20250124",...}` | **CLIENT** — *you* execute | (Anthropic-schema **client** tool) |
| **`text_editor`** | `{"type":"text_editor_20250124",...}` | **CLIENT** | (client) |
| **`computer_use`** | `{"type":"computer_20250124",...}` | **CLIENT** | (client) |
| **`memory`** | Anthropic-schema | **CLIENT** | (client) |
| your custom tools | `{"name":...,"input_schema":{...}}` | **CLIENT** | (client) |

> **Key correction to a common misconception:** `bash`, `text_editor`, `computer_use`, and `memory` are **NOT server-run.** They are *Anthropic-schema **client** tools* — Anthropic publishes the schema and trains Claude on it, but **your application still executes the call and returns a `tool_result`.** [E, overview: "Anthropic-schema client tools"]. Only `web_search`, `web_fetch`, `code_execution`, `tool_search`, `advisor`, and the MCP connector truly run on Anthropic's infrastructure. [E, overview]

### 1.2 The server tool wire shape (one `/v1/messages` call)
When a server tool runs, the response carries **two paired blocks in the same assistant turn**, matched by `tool_use_id` (not by position): [E, server-tools]

```jsonc
// 1. the call
{ "type": "server_tool_use", "id": "srvtoolu_01A2B3...", "name": "web_search",
  "input": { "query": "..." } }
// 2. the result (immediately follows, same turn)
{ "type": "web_search_tool_result", "tool_use_id": "srvtoolu_01A2B3...",
  "content": [ { "type": "web_search_result", "url": "...", "title": "...",
                 "encrypted_content": "Eqgf...", "page_age": "April 30, 2025" } ] }
```
- The `srvtoolu_` id prefix distinguishes server calls from client `toolu_` calls. [E]
- **You never construct a `tool_result` for these.** The result block is produced by Anthropic and appears inline. [E]
- The loop "may repeat multiple times throughout a single request" — multiple search→result pairs can appear before the final cited `text`. [E, web-search-tool]
- Final answer arrives as `text` blocks with `citations[]` (`web_search_result_location`, carrying `encrypted_index` + ≤150-char `cited_text`). [E]
- `stop_reason: "end_turn"` when the turn completes normally. [E]

### 1.3 `pause_turn` — the server-side loop's "I'm not done" signal
- When the server-side agentic loop runs long, the API returns **`stop_reason: "pause_turn"`** instead of `end_turn`. [E, server-tools]
- **Continue by re-sending the assistant content back as-is** (append the response as an `assistant` message, POST again, **same `tools` array**). A paused turn can end on a `server_tool_use` block whose tool *hasn't run yet*; dropping that tool from the continuation is a **validation error**. [E]
- A continuation can itself pause again; loop until you get a non-`pause_turn` stop reason, capping retries. [E]
- Batch API uses a higher per-turn iteration cap, then also returns `pause_turn`. [E]

### 1.4 Mixing server tools + client tools in one turn (THE load-bearing rule for us)
If Claude calls a server tool **and** a client tool in the same parallel group: [E, server-tools]
- **The API does NOT run the server tool.** It returns immediately so you can run the client tool first.
- `stop_reason` is **`"tool_use"`** (not `pause_turn`).
- `content` has the `server_tool_use` block **without** its result block (unfinished) + the client `tool_use` block.
- You detect the deferred server call by: *a `server_tool_use` id with no matching result block in the response.*
- You reply with a user message of **only `tool_result` blocks** for the client calls; the API then runs the deferred server tool and continues. The next response **begins with the server tool's result block** answering the earlier `srvtoolu_` id.

> **This is precisely the contract llamafile should mirror** [I]: *server_run tools auto-execute and continue; any client-declared tool stops the turn and is returned.* We already emit `stop_reason:"tool_use"` + Anthropic `tool_use` for client tools today (verified live, doc 05 addendum) — server-side tools is the *other half* of the same rule.

### 1.5 Streaming event sequence
[E, web-search-tool / server-tools "Streaming server-tool events"]
```
message_start
content_block_start  (text: "I'll search…")
content_block_start  (index n, server_tool_use, name web_search)
content_block_delta  (input_json_delta: streamed query JSON)
   ── pause while the search executes (server-side) ──
content_block_start  (index n+1, web_search_tool_result, content=[...])  ← arrives COMPLETE, no deltas
content_block_start  (text + citations…)
message_delta / message_stop
```
A directly-called `server_tool_use` streams like a client `tool_use` (start + `input_json_delta`); **its result block arrives whole in one `content_block_start`, with no deltas.** [E]

### 1.6 Declaration, accounting, control knobs
- **Declaration is versioned `type` strings**, e.g. `web_search_20250305` (basic), `web_search_20260209` (adds dynamic filtering via code-exec), `web_search_20260318` (adds `response_inclusion`). [E, web-search-tool]
- **`max_uses`** caps searches per request; over-cap yields a `web_search_tool_result` *error* block (`max_uses_exceeded`). Tool errors still return HTTP 200 with an error block. [E]
- **Usage accounting:** `usage.server_tool_use.web_search_requests`; web search billed **$10 / 1,000 searches** + tokens; retrieved results count as input tokens; citation `url`/`title`/`cited_text` do **not**. [E]
- **`allowed_callers`** (`["direct"]` vs code-exec caller) + **ZDR**: basic versions are ZDR-eligible; `_20260209+` aren't by default (they internally use code execution). [E, server-tools]
- **Domain filtering:** `allowed_domains`/`blocked_domains` on the tool object (mutually exclusive); homograph/unicode caveat. [E]

### 1.7 Can a developer declare a CUSTOM server-run tool?
**No.** Server tools are a **fixed, Anthropic-hosted set** addressed by versioned `type` strings; the only thing the developer chooses is *which* of those to enable + their parameters. Custom tools (`name`+`input_schema`) are always **client** tools. The MCP connector is the escape hatch for "server runs a tool you didn't hand-write," but even there Anthropic hosts the connection. [E, overview + server-tools]

> **Implication for llamafile:** we are *not bound* by that limitation — llamafile owns both ends, so "a custom server-run tool" (wiki, browser, MCP-bridged) is trivially expressible. But to stay a drop-in for clients that speak the Anthropic dialect, the *client-facing* shape should look like enabling a named/typed tool, not like inventing arbitrary server execution. [I]

---

## 2. EVIDENCE — OpenAI equivalent (brief)
[E, developers.openai.com/api/docs/guides/tools-web-search + openai.com new-tools posts, accessed 2026-06-29]
- The **Responses API** (launched 2025-03-11) is OpenAI's agent-oriented surface with **built-in/hosted tools**: `web_search`, `file_search`, `computer_use`, `code_interpreter`, and **remote MCP** (added 2025-05-21). Hosted tools run on OpenAI's side and the loop is handled server-side, analogous to Anthropic server tools. [E]
- **Chat Completions** (`/v1/chat/completions`) does **not** have the general hosted-tool/loop mechanism; the only analog is the dedicated **search-preview models** (`gpt-4o-search-preview`, `gpt-4o-mini-search-preview`) which fold search into the completion. Otherwise chat-completions `tools` are client-executed function calls. [E]
- OpenAI explicitly recommends Responses for new agentic work; Chat Completions remains for plain function-calling. [E]

> **Takeaway:** the clean place for a "provider-runs-the-loop" feature is a *Responses-style* surface, not `/v1/chat/completions`. llamafile's `/v1/chat/completions` has no hosted-tool idiom to mirror, so for the OpenAI dialect the honest options are (a) leave it as pure client function-calling, or (b) expose server-side tools only behind an explicit llamafile flag / a separate `/v1/agentic` endpoint — **not** by overloading standard chat-completions semantics. [I]

---

## 3. The repo today (what we'd build on)
[E, read 2026-06-29]
- **`server_tool` base** (`llama.cpp/tools/server/server-tools.h:6`): `{ string name; string display_name; bool permission_write; get_definition(); invoke(json); to_json(); }`. A registry `server_tools` with `setup(enabled)`, `invoke(name,params)`, and `GET/POST /tools` handlers. 8 built-ins registered (`server-tools.cpp:124..719`, dispatch at `:811`). **Adding a `bool server_run = false;` field is a one-line change** and the natural hook. [E]
- **`/v1/messages` + `/v1/messages/count_tokens`** are vendored (`server.cpp:221,238`), with full Anthropic↔OAI conversion (`server-chat.cpp:325`), Anthropic SSE (`server-task.cpp:815`, `input_json_delta` for streamed tool args), and a CC billing-header normalizer (`server-chat.cpp:303`). The conversion already handles `tool_use`/`tool_result` blocks (`server-chat.cpp:420,430`). [E]
- **Client tool round-trip through `/v1/messages` is VERIFIED live** end-to-end (`stop_reason:"tool_use"` → `tool_result` → `end_turn` grounded answer; doc 05 addendum). So the "return client tools as `tool_use`" half already works. [E]
- **The server-side loop does NOT exist yet** — it runs only in the browser today; building it is README step 2 (~200 lines, `runAgentLoop()` sketch in doc 05 §C9). **Server-side tools is a *mode of that loop*, so it cannot precede it.** [E/I]

---

## 4. Should llamafile do this? — decisive analysis

### 4.1 The win (real, but bounded)
- **Thin clients get rich tools with zero client loop.** `curl`, a minimal web UI, embedded apps, and — most relevant — **our own multi-agent sub-agents** (doc 02) can call wiki/browser/coding tools by just enabling them, without each re-implementing the dispatch loop. The orchestrator can hand a researcher sub-agent "you have wiki+browser" and get back a finished answer. [I]
- **Mirrors the hosted `web_search` UX** developers already expect from Anthropic/OpenAI — "turn on the tool, get cited answers." Familiar and ergonomic. [I]
- **Fewer round-trips for fast embedded tools.** wiki/ZIM lookup is an in-process function call (doc 01); bouncing it out to the client and back is pure latency. Running it inline is strictly better for *those* tools. [I]

### 4.2 The conflict (this is why it must be opt-in + scoped)
- **Agentic clients run their own loop + own tools.** Claude Code, opencode, Cline, etc. send *their* tools as request `tools[]` (client/custom tools) and expect them back as `tool_use`. If llamafile auto-ran anything in that array, it would (a) execute tools it has no implementation for, or (b) silently run a llamafile tool the client expected to run itself → **broken client, confusing divergence, possible data loss** (e.g. client's `write_file` vs ours). [I]
- **Therefore the auto-run set MUST be disjoint from the client's request tools.** Safe rule [I]:
  1. **Client-declared tools** (`tools[]` in the request, custom `input_schema` or Anthropic typed) → **always passthrough** = returned as `tool_use`, `stop_reason:"tool_use"`. Never auto-run. (Exactly Anthropic's mixed-turn rule, §1.4.)
  2. **llamafile registry tools** flagged `server_run` and enabled via the mode flag → **injected into the rendered tool list** so the model can call them, **auto-executed inline**, loop continues. Never returned to the client as something to run.
  3. **Name collision** (client declares a tool whose name equals an enabled server tool) → **client wins**; do not inject/auto-run ours. (Prevents silent hijack.)
- **Default OFF.** A vanilla `/v1/messages` request behaves exactly as today (client tools passthrough), preserving the verified CC drop-in. Server-side tools only activate when the operator opts in. [I]

### 4.3 How to detect/decide — the mechanism (recommended)
**A per-tool flag + one mode switch, scoped to embedded tools, on the existing endpoints.** [I]

- **`server_tool::server_run` (new bool).** Default `false`. Set `true` for *safe, in-process, side-effect-light* tools: `wiki_search`, `wiki_get_article`, `browser_*` (read/fetch), MCP read-only tools. **Leave `false` for `write_file`/`edit_file`/`apply_diff`/`exec_shell_command`** — auto-running mutating/code-exec tools server-side on behalf of an unknown client is a footgun (see §5). Code-exec parity with Anthropic's `code_execution` is explicitly *out of scope* for auto-run v1.
- **`--server-side-tools[=wiki,browser,...]`** (default off). Enables the mode and selects which `server_run` tools to inject+auto-run. Reuses the existing `--tools`/`--agent` gating philosophy (doc 01 §2).
- **Endpoint choice:** **overload `/v1/messages` and `/v1/chat/completions` behind the flag** rather than minting `/v1/agentic`. Rationale: (a) the whole point is thin clients that already speak these dialects; a new endpoint defeats "drop-in"; (b) with default-off + the namespace rule, overloading is non-breaking; (c) doc 01/05 already earmark a server-side loop reachable from these surfaces. *Keep the door open* to also expose it as an explicit `/v1/agentic/chat` for callers who want loop semantics without ambiguity, but that's additive, not the primary path. [I]
- **Optional Anthropic-faithful declaration (later):** accept `{"type":"web_search_...","name":"web_search"}`-style entries in `tools[]` and map them onto llamafile registry tools (e.g. our wiki as the "web_search" backend). This makes llamafile a closer drop-in for code written against hosted search — but it's a v2 nicety, not required for v1. [I]

### 4.4 Complexity: faithful blocks vs simple v1 (recommend the simple v1)
| Aspect | Faithful (Anthropic-exact) | **Recommended v1** |
|---|---|---|
| Result surfacing | emit `server_tool_use` + typed `*_tool_result` blocks, `srvtoolu_` ids, paired by `tool_use_id` | run inline; **return final assistant message**; optionally surface "🔎 searched X" as ordinary `text`/`thinking` blocks |
| Long turns | implement `pause_turn` + resend-as-is contract (§1.3) | **internal `max_turns`/`max_uses` cap**; just finish or return final text; no client pause protocol |
| Streaming | inject server-tool SSE events mid-stream (§1.5) | stream the final answer normally; (optionally) stream a visible "searching…" text block |
| Citations | `web_search_result_location` + `encrypted_index` round-trip | inline plain-text citations in the answer; no encryption/round-trip |
| Accounting | `usage.server_tool_use.*` | optional: add a llamafile-specific `x-llamafile-tools-run` count; don't fake Anthropic's field |
| Net new code | large, brittle, must track Anthropic versions | **~the step-2 loop + 1 bool + 1 flag + tool-list injection** |

The faithful path is a standing maintenance burden (versioned `type` strings, encrypted citations, ZDR/`allowed_callers`, pause protocol) that buys a thin client *nothing it asked for* — a thin client wants the answer, not to replay `server_tool_use` blocks. **v1 = loop + final message.** Promote individual pieces (e.g. visible tool blocks, then faithful blocks) only when a concrete client needs them. [I]

### 4.5 Interaction with the existing client-tool path (don't break CC-dropin)
- The client-tool passthrough path is **unchanged and primary**; server-side tools is an *additive* branch in the same loop. With default-off + the namespace/collision rules (§4.2), a CC session sees today's exact behavior. [I]
- When *both* are active in one turn (client tool + our server tool), follow §1.4 precisely: **stop and return the client `tool_use`**, defer our server tool, run it after the client's `tool_result` comes back. This is the one place the loop must *not* greedily finish. Getting this right = CC can layer its own tools on top of llamafile's wiki/browser without conflict. [I]

---

## 5. Failure modes to design against
[I, grounded in §1 + doc 05 §C8]
1. **Silent divergence / hijack** — a client expects to run a tool that llamafile silently ran. *Mitigation:* never auto-run client-declared tools; on name collision the client wins; auto-run only the disjoint `server_run` registry set; default off.
2. **Prompt-injection via auto-run browser/wiki** — auto-fetched web/wiki content is untrusted and can carry instructions; running the loop server-side means *llamafile* ingests it without a human in the loop. *Mitigation:* keep `server_run` tools read-only; never flag mutating/exec tools `server_run`; apply the doc-04 browser guardrails (domain controls, Readability sanitization); treat tool output as data, not instructions.
3. **Unbounded cost/loops server-side** — the server now owns the loop, so a runaway model burns *our* compute and a slot. *Mitigation:* hard `max_turns` + per-tool `max_uses` caps (mirror Anthropic's `max_uses`); a wall-clock budget; surface a `max_uses_exceeded`-style terminal note; the loop holds a continuous-batching slot only while generating (doc 02), so cap turns to avoid slot starvation.
4. **Mutating tools auto-run without approval** — `write_file`/`exec_shell_command` server-side, on behalf of an anonymous HTTP client, with no cwd-jail (doc 05 §C8 says the jail isn't built yet) = remote code execution. *Mitigation:* `server_run=false` for all mutating tools in v1; require the path-jail + approval modes (doc 05 §C8) before any mutating tool could ever be considered.
5. **Drop-in regressions** — overloading `/v1/messages` could perturb the verified CC round-trip. *Mitigation:* default off; integration test the CC path with the flag both off and on (client tools must still passthrough identically).
6. **Faking accounting** — emitting Anthropic's `usage.server_tool_use` for locally-run tools would mislead cost trackers. *Mitigation:* don't; use a clearly llamafile-namespaced counter if any.

---

## 6. Recommendation + placement in the build order

**Worth it: yes, in the bounded form above.** It is a small, high-leverage feature *if and only if* it rides the step-2 loop and stays opt-in + scoped to safe embedded tools. It directly serves this project's own multi-agent sub-agents and the "thin client gets wiki/browser for free" story, and it mirrors a UX developers already know. It is **not** worth it as a faithful re-implementation of Anthropic's hosted-tool wire protocol — that's a large brittle surface with no payoff for thin clients. [I]

**Build order (refines README §"Suggested build order"):**
1. (step 1) wiki `server_tool`s — *something worth auto-running.*
2. (step 2) **server-side agentic loop** (`runAgentLoop`, doc 05 §C9) — **prerequisite.**
   - **2.5 (this doc): server-side tools.** Add `server_tool::server_run`; add `--server-side-tools[=...]` (default off); in the loop, inject enabled `server_run` tools into the rendered tool list, auto-execute them inline, continue the turn; **passthrough all client-declared tools** (§4.2); honor the mixed-turn rule (§1.4); enforce `max_turns`/`max_uses` (§5). v1 returns the final assistant message (no faithful `server_tool_use` blocks).
3. (step 4) browser tools → become additional `server_run` candidates.
4. **Later / optional:** Anthropic-faithful surfacing — emit real `server_tool_use`/`*_tool_result` blocks, accept `{"type":"web_search_..."}` declarations mapped to our backends, `pause_turn`, citations. Do this per-demand, not speculatively.

**One-line summary:** *Add a `server_run` flag + an opt-in mode that lets the step-2 loop auto-run llamafile's own read-only tools inline while passing every client tool straight through — same rule Anthropic uses to mix server and client tools — and skip the faithful `server_tool_use` block protocol until a client actually needs it.*

---

## 7. The complement: llamafile as an MCP **SERVER** (for external loop-drivers)

Server-side tools (this doc's main subject) serve **thin clients that *don't* run a loop**. They are the *wrong* answer for the opposite case: **external agentic clients that run their OWN loop and tools** (Claude Code, opencode, Cursor). For those, implicit server-side execution conflicts head-on (§4.2) — CC wants to *list* tools, have *its* model decide, and execute them in *its* loop. The clean way to give such a client llamafile's embedded data is the inverse: **llamafile exposes its own tools OUT, as an MCP server.** [I]

### 7.1 The organizing frame — one handler, three exposure surfaces
Every llamafile tool is a single `server_tool` (`get_definition()` + `invoke()`, §3). That one implementation is exposed three ways, picked by *who drives the loop*: [I]

| # | Surface | Who drives the loop | Consumer | Mechanism |
|---|---|---|---|---|
| 1 | **Internal registry** | llamafile | llamafile's own loop + web UI | `GET/POST /tools` (exists today, `server-tools.cpp`) [E] |
| 2 | **Server-side tools** | **llamafile** (inline, under the hood) | thin clients, our sub-agents | this doc §1–6: `--server-side-tools`, auto-run `server_run` tools |
| 3 | **MCP server** | **the external client** (CC/opencode/Cursor) | external agentic clients | `--mcp-server`: expose tools over MCP; client lists, model calls, **client executes** via MCP back into llamafile |

**Same `invoke()` underneath all three.** The surfaces differ only in *who owns the agentic loop and the execute step.* This is the doc's unifying picture: server-side tools and MCP-server are the two halves of "give external code llamafile's tools," split by whether llamafile or the client runs the loop.

### 7.2 The MCP duality — llamafile is both host/client AND server
llamafile should play **both** MCP roles (they are independent and both useful): [I, grounded in doc 01 §5]
- **MCP host/client (doc 01 §5, P3 ✅):** llamafile connects *out* to external MCP servers and bridges their tools *into* its registry — for when **llamafile drives the loop** (surfaces 1 & 2). Tools flow *inward*.
- **MCP server (this section):** llamafile exposes its *own* tools (`wiki_search`, `wiki_get_article`, later `browser_*`, the coding tools) *out* to external loop-drivers — for when **the client drives the loop** (surface 3). Tools flow *outward*.

These compose: a single llamafile can bridge a third-party MCP server inward *and* re-expose everything (its own + bridged) outward to CC. [I]

### 7.3 Concrete: Claude Code over the embedded ZIM (offline path)
```
claude mcp add wikipedia -- /path/llamafile --mcp-server --zim simplewiki.zim
```
→ CC spawns llamafile as a stdio MCP server, calls `tools/list` (gets `wiki_search`, `wiki_get_article`), and from then on **CC's model** decides when to call them; **CC executes** the call via the MCP protocol; llamafile answers by querying the embedded ZIM (doc 01 P1 ✅) and returns the article text — **all inside CC's own loop.** [I]

**Contrast with CC's built-in `WebSearch`:** that hits Anthropic's hosted/online search (§1). The llamafile MCP server is the **offline / local / private** counterpart — air-gapped Wikipedia (and later local browser + repo tools) as MCP tools, no network, no per-search billing. That is a distinctive "portable offline knowledge server in one APE binary" story. [I]

### 7.4 Implementation — cheap, it's the inverse of P3
MCP-server mode is the **mirror image of the validated P3 MCP *client*** (doc 01 §7): same **newline-delimited JSON-RPC 2.0 over stdio**, proven under cosmocc 4.0.2 via `posix_spawn`/`pipe2` — but here llamafile is the *callee*. It is a thin adapter that wraps the existing `server_tool` handlers: [I, E for P3]
- `initialize` → advertise capabilities; `tools/list` → emit each enabled tool's `get_definition()` (schema already exists); `tools/call name args` → `server_tools::invoke(name, args)` (`server-tools.cpp:811`) → wrap result as an MCP `content` payload. ~the P3 client's ~110 lines, inverted.
- **Transports:** **stdio primary** (for `claude mcp add -- cmd`, the dominant case); **HTTP/SSE secondary** via vendored `cpp-httplib` `SSEClient`/server (doc 01 §5) for remote/persistent servers.
- **Gating/safety:** same `--tools`/`--agent` enablement and the §5 cautions — expose read-only tools (`wiki_*`, browser fetch) freely; mutating/exec tools (`write_file`, `exec_shell_command`) require the doc-05 §C8 path-jail + approval before exposure. Note MCP-server mode hands *execution intent* to an external client, so treat its calls as untrusted input.

### 7.5 Decision guidance (which surface for whom)
[I]
- **Thin client (curl, basic UI) or OUR own sub-agents** → **server-side tools** (surface 2): llamafile runs the loop, returns the answer.
- **External agentic client (Claude Code, opencode, Cursor) that runs its own loop** → **MCP server** (surface 3): llamafile exposes tools; the client's model decides and executes.
- **llamafile's own web UI / CLI agent** → **internal registry** (surface 1): `POST /tools`.
- **All three call the same `invoke()`** — adding a surface is an adapter, not a new tool implementation.

**Build-order placement:** MCP-server mode slots alongside the MCP host (README step 3) — once the `server_tool` handlers + the P3 stdio JSON-RPC plumbing exist, the server direction is a small additional adapter. It is **independent of** the server-side-tools loop (§2.5) and can land in either order; together they cover thin-client *and* external-agentic-client consumption of llamafile's embedded tools.

---

## Sources (accessed 2026-06-29)
- Anthropic — Tool use overview: https://platform.claude.com/docs/en/agents-and-tools/tool-use/overview
- Anthropic — Server tools (shared mechanics, pause_turn, mixing, ZDR/allowed_callers): https://platform.claude.com/docs/en/agents-and-tools/tool-use/server-tools
- Anthropic — Web search tool (declaration, blocks, streaming, usage, max_uses): https://platform.claude.com/docs/en/agents-and-tools/tool-use/web-search-tool
- Anthropic — Advanced tool use (tool search / programmatic tool calling): https://www.anthropic.com/engineering/advanced-tool-use
- Anthropic — Stop reasons & fallback (pause_turn vs tool_use): https://platform.claude.com/docs/en/build-with-claude/handling-stop-reasons
- OpenAI — Web search tool / Responses vs Chat Completions: https://developers.openai.com/api/docs/guides/tools-web-search
- OpenAI — New tools for building agents (Responses API hosted tools): https://openai.com/index/new-tools-for-building-agents/
- Repo: `llama.cpp/tools/server/server-tools.h:6`, `server-tools.cpp:124..811`, `server.cpp:177,221,238`, `server-chat.cpp:303,325,420,430` (read 2026-06-29).
</content>
</invoke>
