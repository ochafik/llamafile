# llamafile as an Agentic Coding Tool + Long-Context & Compaction — design (2026-06-29)

> Scope: how llamafile becomes a local agentic coding tool (an opencode/Claude-Code-style CLI **and/or** a drop-in backend), plus the long-context & compaction mechanics for that and the multi-agent system.
>
> **Cross-references:** general multi-agent principles live in `MULTIAGENT-BEST-PRACTICES-2026.md` (NOT re-derived here). Tool/registry architecture lives in `MCP-WIKIPEDIA-DESIGN.md`. Web-UI agentic loop lives in `MULTIAGENT-WEBUI-DESIGN.md` / `BROWSER-TOOL-DESIGN.md`. This doc focuses on the **coding-agent + long-context/compaction** specifics.
>
> **Convention:** **[E]** = EVIDENCE (read in repo with file:line, or a cited URL accessed 2026-06-28/29). **[I]** = INFERENCE (reasoning over evidence). Repo line numbers are against the tree at `/Users/ochafik/github/llamafile-upgrade2026` on 2026-06-29.

---

## 0. The headline findings (read this first)

1. **The Anthropic Messages API is ALREADY in the vendored server.** `POST /v1/messages` and `POST /v1/messages/count_tokens` are wired (`llama.cpp/tools/server/server.cpp:221,238`), with full Anthropic SSE (`server-task.cpp:815 to_json_anthropic_stream`, `input_json_delta` at `:925`), OpenAI⇄Anthropic conversion (`server-chat.cpp:325 server_chat_convert_anthropic_to_oai`), and — the smoking gun that someone already pointed Claude Code at it — a **Claude-Code billing-header normalizer** (`server-chat.cpp:303 normalize_anthropic_billing_header`, matches `"You are Claude Code, Anthropic's official CLI for Claude."`). **→ Claude Code drop-in is ~80% already built**, not a greenfield shim. [E]
2. **The 8 coding tools exist; the server-side agent loop does not.** `read_file, file_glob_search, grep_search, exec_shell_command, write_file, edit_file, apply_diff, get_datetime` are registered in `server-tools.cpp:733-740`. The agentic loop that chains `tool_calls`→execute→re-prompt runs **only in the browser** today (per `MCP-WIKIPEDIA-DESIGN.md` §2). The CLI gap is a **server-side loop + a terminal front-end** — both have most pieces already. [E]
3. **Per-slot/per-request LoRA is supported, but different-adapter slots cannot share a decode batch.** `task.params.lora` → per-slot `slot.lora` (`server-context.cpp:1593-1604`), `/lora-adapters` hot-swap (`server.cpp:240-241`), even activated-LoRA (aLoRA). BUT `can_batch_with()` requires `are_lora_equal()` (`server-context.cpp:289-292`) and the adapter is set **once per batch** (`server-context.cpp:3333-3334`). → concurrent role-specialized agents work, but each distinct adapter forms its own batch group (round-robin, not fused). [E] — make-or-break answer in §A5.
4. **Precomputed-KV bundling is feasible** via `--prompt-cache`/`--slot-save-path` + `/slots/:id?action=restore` (`arg.cpp:1490,3058`; `server-context.cpp:4291-4314`) and the APE zip layer (`llamafile.c:70 llamafile_open_zip`). The win is **shared-prefix reuse across many agents + instant cold-start**, not raw prefill savings. §A4.
5. **Effective context ≪ advertised.** Plan for **~16–32K usable** high-signal tokens on a 35B-A3B-class model even though it advertises 256K/1M. Compaction is not optional — it is the core mechanic. §A1.

**Verdict triplet (details in §C5, §A2, §C7):**
- **Build-vs-import:** *Hybrid* — **BUILD** a thin server-side loop + reuse the existing TUI; **BACKEND** for everyone else; **do NOT vendor** any agent codebase (the proprietary ones are off-limits, the permissive ones are JS/Rust loops that don't fit our C++ server better than our own ~200-line loop).
- **Compaction policy:** append-only history + frozen front prefix + **anchored one-shot compaction at ~70% of a deliberately small (32–64K) working window**, sub-agents return 1–2K distillates, tool-output elision first.
- **CC drop-in:** **feasible today** with minor verification; the Anthropic surface is already vendored.

---

# PART A — Long context & compaction

## A1. Effective-context reality + KV memory math

### Usable context (plan small)
Advertised windows are memory ceilings, not competence ceilings. 2025–26 benchmarks agree the usable fraction is roughly **50–65% of advertised for frontier models, and worse for a 35B-A3B-class MoE**:
- **Context Rot** (Chroma, Jul 2025, research.trychroma.com/context-rot) [E]: accuracy degrades monotonically with input length even on trivial probes; a *single distractor* lowers accuracy; tested Qwen3-235B/32B/8B among 18 models.
- **NoLiMa** (ICML 2025, arXiv:2502.05167) [E]: with keyword-matching removed (semantic lookup — what coding reasoning needs), 10 of 13 models claiming ≥128K drop below 50% of their short-context baseline **by 32K**; effective semantic length was **1–8K** on 2024–25 models.
- **RULER** (NVIDIA) [E]: "only half of models claiming ≥32K effectively handle 32K."
- **Lost in the Middle** (Liu et al., TACL 2024, arXiv:2307.03172) [E]: U-shaped recall → put the task spec, file-under-edit, and failing test at the **start or end**, never mid-prompt.

**Qwen3.6-35B-A3B window** [E, HF card + Qwen3 tech report arXiv:2505.09388]: **256K native (262,144), ~1M extended via YaRN**; MoE 3B active / 35B total; GQA (not MLA); Apache-2.0.

**Design rule [I, grounded above]:**
| Zone | Tokens | Trust |
|---|---|---|
| High-reliability | ≤16–32K curated | use everything |
| Degraded-workable | 32–64K | only if edge-placed + distractors pruned |
| Best-effort/lossy | >64–128K | accepted, mid-context recall unreliable |
| 256K/1M YaRN | — | effectively never fully usable for reasoning-grade work at this size |

→ A coding agent should target a **32–64K working window** and compact aggressively, *not* ride the nominal 256K. This simultaneously improves reliability **and** shrinks KV (see below), letting more agents run concurrently.

### KV-cache memory math
Formula [E, omrimallis.com]: `bytes/token = 2 · n_layers · n_kv_heads · head_dim · bytes_per_elem` (the 2 = K+V; `n_kv_heads` is GQA-reduced — the big lever).

Qwen3-30B-A3B config [E, HF config.json]: 48 layers, 4 KV heads, head_dim 128 → 49,152 elem/token. Qwen3.6-35B-A3B is the same family/shape class [I] → use ~49K elem/token as the estimate:

| KV dtype | bytes/token | per 32K | per 128K |
|---|---|---|---|
| f16 | ~96 KiB | 3.0 GiB | 12.0 GiB |
| q8_0 | ~51 KiB | 1.6 GiB | 6.4 GiB |
| q4_0 | ~27 KiB | 0.84 GiB | 3.4 GiB |

KV quant in llama.cpp: `--cache-type-k/--cache-type-v q8_0|q4_0`; V-quant (and best K) needs Flash Attention `-fa` [E].

**Continuous-batching budget** [E, llama.cpp #11681]: server allocates ONE KV cache of `--ctx-size` at load, split across `-np` slots (`n_ctx_per_seq ≈ n_ctx/n_parallel`). **OOM happens at startup, not mid-run.** To give N agents C tokens: `-np N -c (N·C)`.

**Worked example — 35B MoE q4 weights (~18 GiB) + ~4 GiB overhead = ~22 GiB base on a 96 GB Mac → ~74 GiB for KV** [I]:

| KV dtype | per agent @128K | agents fit | first N to blow 96 GiB |
|---|---|---|---|
| f16 | 12.0 GiB | 6 | **7** |
| q8_0 | 6.4 GiB | 11 | 12 |
| q4_0 | 3.4 GiB | ~21 | 22 |

At a realistic **32K** working window, f16 KV/agent = 3.0 GiB → **~24 concurrent agents fit**; ~70 at q4_0. **Headline: with f16 KV, 7 concurrent 128K agents exceed a 96 GB Mac — but compacting to 32K gets you 24.** KV quant + small windows are the levers; the multi-agent orchestrator should budget KV explicitly. (macOS note: raise `iogpu.wired_limit_mb` or the GPU working-set caps below 96.)

## A2. Compaction strategies — what to adopt

### How leading agents actually do it (2025–26) [E]
- **Claude Code / Anthropic Compaction API** (platform.claude.com/docs/.../compaction, beta `compact-2026-01-12`): default trigger **150K input tokens**; emits a `<summary>` block then **drops all message blocks before the compaction block**; default prompt asks for "state, next steps, learnings." CLI wraps this with a **~16.5% reserved buffer** (the "context left until auto-compact" counts down to that). `/compact [instructions]` for manual + retention focus. Keeps: accomplishments, WIP, modified paths, next steps, constraints, recent diffs, conventions. Drops: resolved debug loops, tangents, redundant tool output.
- **Anthropic "effective context engineering"** (anthropic.com/engineering/effective-context-engineering-for-ai-agents) names three techniques: **compaction**, **structured note-taking** (agent writes `NOTES.md`/todo persisted outside context), **sub-agents** returning a distilled **1–2K token** summary. "Lowest-hanging fruit = clearing old tool calls/results." Multi-agent post: token usage explains ~80% of performance variance → distill aggressively.
- **opencode** [E]: compact ~75% (tunable), tool-output pruning protects last 40K, prunes only if ≥20K prunable, keeps the call record but drops the body.
- **Cline** [E]: auto-condense at ≥80%, effective size `max(window−40K, 0.8·window)`; `new_task` = handoff/sub-agent.
- **Cursor** [E]: Composer is *trained* to self-summarize; writes full history to a file before summarizing (two-tier memory) so it can search back.
- **aider** [E]: `/clear` manual + `max_chat_history_tokens` auto-summary; core strategy is the **repo map** (PageRank over the dependency graph, `--map-tokens` default 1K) — explicit user-curated context, not autonomous compaction.

**Cross-cutting [I]:** three converging mechanisms (summarize-and-restart, tool-output pruning, external memory files); the real design knob is the **reserved buffer**; triggers are drifting earlier (95%→75–80%) because late compaction yields worse summaries; **two-tier memory** (cheap recovery path to discarded detail) and **sub-agents-as-compaction** are emerging best practice.

### Recommendation for llamafile

**(a) Single long coding session:**
- **Working window = 32–64K** (not 256K — §A1). Set `n_ctx` accordingly per slot.
- **Trigger compaction at ~70% of the working window** (earlier than CC's effective ~83% because our model is weaker and our window smaller — leaves room for a good summary + the next turn). Reserve a **~25% buffer**.
- **Elide tool output first** (the cheapest win): keep the tool *call record* + a one-line result; drop large file dumps/command output bodies once they're >N turns old (protect the last ~8–16K tokens of raw context). This often defers full compaction entirely.
- **On full compaction:** emit a structured summary — *goal, decisions made, files modified (paths), current WIP, next steps, constraints/conventions, last diff* — then continue. Keep the system prompt + tool defs + `AGENTS.md`/`CLAUDE.md` verbatim (frozen prefix, §A3).
- **Two-tier memory:** before compacting, append the dropped raw history to a session `.notes`/scratch file in cwd so the agent can `grep_search`/`read_file` it back (we already have those tools — `server-tools.cpp:271,124`). This is free given the tool surface.
- **External memory file:** adopt `AGENTS.md` (the cross-tool de-facto standard — opencode/Codex/Cursor read it) as the durable project-context file the agent reads at session start and updates. (Optionally also honor `CLAUDE.md` for CC-compat.)

**(b) Multi-agent orchestrator (cross-ref `MULTIAGENT-BEST-PRACTICES-2026.md`):**
- **Sub-agents are the primary compaction strategy.** Each searcher/verifier/coder runs in its **own slot with its own KV window** and returns a **1–2K token distillate** to the orchestrator — the orchestrator never sees the sub-agent's raw tool spew. This is both a context strategy and a KV strategy (§A1: keeps each slot small → many agents fit).
- **Orchestrator context budget:** keep the orchestrator's own window small (it holds the plan + distillates, not raw work). Persist the plan to a memory file before it can be truncated.
- **Distillation contract:** sub-agent final message = `{outcome, artifacts (paths/line-refs), key findings, open questions}` — the same shape as the file-path-only return discipline this repo already uses for its own agents.

## A3. Compaction × prefix/KV-cache reuse (and a policy)

**How prefix caching works here** [E]: per-slot, `cache_prompt` default-on (`arg.cpp:3020`); the server token-by-token compares the new prompt vs the slot's cached tokens, skips the matched prefill, trims the divergent suffix. `--cache-reuse N` (`arg.cpp:3026`) can KV-shift *unchanged chunks after divergence*. `--cache-ram`/`-cram` (`arg.cpp:1339`) spills idle slots to host RAM. `--slot-save-path` + `/slots/:id?action=save|restore|erase` (`arg.cpp:3058`; `server-context.cpp:4308-4314`) persist to disk. `--prompt-cache FNAME` / `--prompt-cache-all` / `--prompt-cache-ro` (`arg.cpp:1490-1507`).

**The interaction (the trap)** [E, Manus/ankitbko]: attention is causal — KV at position *i* depends on tokens 0..*i*. **Compaction is a mid-history rewrite**: the first divergent token *D* invalidates all KV ≥ *D*. Even textually-unchanged later turns can't be reused because their positions shifted.
- **Cheap case:** if you keep the front prefix (system + tool defs + `AGENTS.md`) byte-identical and **append** the summary, only the summary tokens are prefilled.
- **Expensive case:** the next request after compaction must re-prefill from *D* to the end (front reused, compacted body fresh) — unavoidable in any exact-prefix scheme.

**Policy (decisive):**
1. **Freeze a stable front prefix.** System prompt + tool schemas + static project file must be byte-identical every request — never inject timestamps/git-status/usernames into it (put volatile data in user messages). (This is also what makes §A4 bundled-KV work.)
2. **Append, don't rewrite.** History is append-only; compaction appends a summary and continues.
3. **Anchored one-shot compaction.** Summarize the *oldest stable region once*, freeze the summary, keep recent turns raw. Do **not** re-summarize the same region every cycle (that moves *D* earlier repeatedly → re-prefill each time).
4. **Compact in batches, infrequently** — amortize the one-time re-prefill.
5. **Mask tools, don't remove them** — changing the tool set changes the prefix and nukes everything; disable via the registry/logit side, keep the schema stable.
6. **Deterministic serialization** of tool args/results (sorted keys) — nondeterministic ordering silently breaks the cache.
7. **Elide tool-output bodies at compaction boundaries only**, not continuously (mid-history edits are themselves rewrites).
8. **llama.cpp specifics:** pin `id_slot` per agent (deterministic slot→cache mapping), set `--cache-reuse N>0`, size `--cache-ram`, use `--keep` to protect the front prefix during any context-shift. Default is now `--no-context-shift` (errors instead of silently dropping) — good; it forces explicit compaction.

Quantified payoff [E, community]: stable prefix → ~85% cache-hit, TTFT ~2727ms→~953ms; perturbed prefix → 0% hit.

## A4. Bundled precomputed system-prompt KV (assessing the user's idea)

**Idea:** precompute the KV-cache/session file for the agents' *fixed* system prompts (orchestrator/ideator/researcher/verifier prompts + tool schemas) offline, bundle it inside the APE zip alongside the model + ZIM, and load it on launch so the long role prefixes are pre-warmed — zero prefill for the system prompt, composing with shared-prefix KV reuse across continuous-batching slots.

**Feasibility: YES, the machinery exists.** [E]
- **Save/load API:** `llama_state_seq_save_file` / `llama_state_load_file` (llama.cpp), surfaced as `--prompt-cache FNAME` (`arg.cpp:1490`), `--prompt-cache-ro` (read-only, `:1504`), and the server's `--slot-save-path` + `POST /slots/:id?action=restore` (`server-context.cpp:4291,4311`; result type `server_task_result_slot_save_load` at `server-task.cpp:1529`). The on-disk format is llama.cpp's session/state binary (KV tensors + token list + metadata). **It can be produced offline at build time** by running the model once over each system prompt and saving the slot — then shipped.
- **APE embedding:** bundle the `.bin` state files in the zip exactly like the model/ZIM and open them via `llamafile_open_zip` (`llamafile.c:70`), or the `zim_open_fd`-style `{fd, base_offset}` accessor sketched in `MCP-WIKIPEDIA-DESIGN.md` §4. On launch, restore into the relevant slots before serving.

**Hard constraints — the bundled KV is valid for exactly ONE (model, prompt, settings) tuple** [E/I]. All must match at load or the KV is garbage:
- exact **model file + quant** (same GGUF weights),
- exact **tokenizer** (same token ids for the prompt),
- exact **prompt token ids** (byte-identical system text → same tokens),
- **context/RoPE params** (n_ctx, rope scaling/YaRN settings),
- **KV cache dtype** — `--cache-type-k/v` f16 vs q8_0 must match what was used to produce it,
- **same llama.cpp build/state-format version** (the session format is versioned; a mismatched build refuses to load).
→ **Versioning/validation is mandatory:** stamp the bundle with a fingerprint (model hash + quant + tokenizer hash + prompt hash + ctx/rope + KV dtype + state-format version) and **fall back to runtime prefill if it doesn't match** (never load mismatched KV). The `--prompt-cache` path already validates token-prefix on load and re-prefills the remainder, so a soft-fallback is natural.

**Size** [E/I]: ~96 KiB/tok f16 (~51 KiB q8). A few-K-token system prompt → **a few hundred MB at most** (e.g. 4K tokens × 4 roles × 96 KiB ≈ 1.5 GiB f16, ~0.8 GiB q8 — bundle q8). Fine next to a 22 GB model. Tighten by KV-quantizing the bundle.

**Verdict — worth it, but for the right reason.** Plain prefill of a few-K-token system prompt is *cheap* (sub-second), so the cold-start saving alone is marginal. **The real, compounding wins are:**
1. **Instant cold-start** of every agent (no first-request prefill stall) — matters for a snappy CLI and for spawning many short-lived sub-agents.
2. **Shared-prefix KV reuse across ALL continuous-batching slots** — every agent shares the *same warm role prefix*, so the orchestrator + N sub-agents all start from the cached front prefix instead of each prefilling it. At N agents this is an N× prefill saving, and it composes directly with the §A3 frozen-prefix policy.
3. **Determinism/reproducibility** — the bundled binary behaves identically on first run on any machine.

**Recommendation:** ship it as an **optional optimization with a strict fingerprint + soft fallback**, KV-quantized, for the *fixed role system prompts only* (not per-session content). Treat it as the build-time materialization of the §A3 "frozen front prefix." Prior art: the **golem project already has KV session-file machinery** (`golem_kv_session`, see MEMORY) — reuse its save/load discipline (it is not in this repo tree; it's a separate fork — confirmed absent here by grep). Don't over-invest before measuring: if first-request prefill of the role prompts is <1s and only a couple of long-lived agents run, runtime prefix-cache warming on the first request gets ~90% of the benefit for ~0% of the bundling complexity. **The bundle earns its keep when (a) many short-lived sub-agents spawn, or (b) the role prompts + tool schemas are large (multi-K tokens).**

## A5. Bundled QLoRA role adapters (assessing the user's idea)

**Idea:** ship one base model + several small role-specialized LoRA adapters (orchestrator/ideator/researcher/verifier/coder), selectable per agent — role specialization from one base model in one portable binary.

**Feasibility: YES for selection; the make-or-break concurrency answer has a caveat.** [E]
- **LoRA support is present:** `--lora` / `--lora-scaled` (`arg.cpp:2520-2539`), `--lora-init-without-apply` (load-but-don't-apply, `:3220`), runtime hot-swap via `GET/POST /lora-adapters` (`server.cpp:240-241`), and **per-request adapter selection + scaling**: a request's `lora` field → `parse_lora_request` (`server-common.cpp:131`) → per-slot `slot.lora` via `construct_lora_list` (`server-context.cpp:1579-1604`). Even **activated-LoRA (aLoRA)** is supported. So *different slots can hold different adapters at the same time* — role specialization works.
- **Bundle the `.gguf` LoRA files in the APE zip** (tens of MB each) and load via the zip layer (`llamafile.c:70`), or `--lora` pointing at `/zip/...` paths.

**The make-or-break caveat (decisive):** LoRA is applied **once per decode batch, context-globally** — `common_set_adapter_lora(ctx_tgt, slot_batched->lora)` at `server-context.cpp:3333-3334`, and the scheduler only co-batches slots whose adapters match: `can_batch_with()` requires `are_lora_equal(lora, other_slot.lora)` (`server-context.cpp:289-292`). **→ Slots running *different* role adapters CANNOT be fused into the same continuous-batching batch.** They are decoded as **separate batch groups, round-robin**, not mixed. Consequences:
- Concurrent role-specialized agents **work**, but you lose intra-batch throughput across adapters: N distinct adapters ⇒ up to N serialized batch passes per scheduler tick (agents sharing an adapter still batch together).
- **Switching a slot's adapter clears that slot's KV cache** (`server-context.cpp:1596-1604 lora_should_clear_cache` → `slot.prompt.tokens.clear()`), unless it's aLoRA. So don't thrash adapters on a slot; pin one role per slot for the agent's lifetime.

**Interaction with §A4 bundled-KV (important):** a precomputed KV is valid only for the **exact model state including the active LoRA**. So either (a) precompute the role-prompt KV **on the base model with no adapter** (then the adapter is applied at decode over a base-model KV — only valid if the adapter wasn't active during prefill, which changes results), or more correctly (b) **precompute one KV bundle per (role-adapter + role-prompt) pair**. (b) is the right design: each role ships {adapter.gguf + its prompt-KV.bin computed *with that adapter active*}, fingerprinted together. Don't mix a base-model KV with an active adapter.

**Verdict — feasible and cheap to ship; per-slot LoRA IS supported, which is the make-or-break.** Adapters are tens of MB, so bundling several is negligible next to the model. Role specialization from one base model in one binary is real. The cost is **throughput, not correctness**: many *different-adapter* agents serialize at the batch level. **Recommendation:** support it, but (1) prefer **fewer adapters with more agents per adapter** (agents sharing a role batch efficiently); (2) consider whether role specialization is better done with **prompt/system-message specialization alone** (free, fully batchable) before reaching for LoRA — LoRA earns its keep only when prompt-only steering is insufficient; (3) if used, pair each adapter with its own §A4 KV bundle.

---

# PART B — Agentic coding tools landscape (June 2026) → import or build?

## B4. The field (license = the gating question for vendoring into Apache/MIT)

All accessed 2026-06-28/29; licenses read from each repo's LICENSE.

| Tool | License | Vendorable into Apache/MIT? | Lang | BYO local model | Needs model tool-calling? |
|---|---|---|---|---|---|
| **aider** | **Apache-2.0** [E] | ✅ | Python (LiteLLM) | `OPENAI_API_BASE` | **No** — parses SEARCH/REPLACE text (easiest local fit) |
| **opencode** (sst) | **MIT** [E] | ✅ | TS (Bun) | `opencode.json baseURL` | Yes (OpenAI fn-calling + SSE + `/models`) |
| **Continue** | **Apache-2.0** [E] | ✅ but **repo archived ~Jun 2026** (Cursor acq.) | TS+Kotlin | `config.yaml apiBase` | Yes |
| **Cline** | **Apache-2.0** [E] | ✅ | TS/Node | "any OpenAI-compatible" + Ollama/LM Studio | Yes |
| **Goose** (Block) | **Apache-2.0** [E] | ✅ (Rust core) | Rust + TS | OpenAI-compat / Ollama base URL | Yes (tools mostly via MCP) |
| **Codex CLI** (OpenAI) | **Apache-2.0** [E] | ✅ | **Rust** | `[model_providers]` + `--oss` | Yes |
| **Gemini CLI** | **Apache-2.0** [E] | ✅ | TS/Node | **No** OpenAI/Anthropic (Gemini wire only) | Yes |
| **Claude Code** | **Proprietary** [E] | ❌ | TS bundled via Bun | Anthropic `/v1/messages` only (`ANTHROPIC_BASE_URL`) | Yes |
| **Cursor CLI** | **Proprietary** [E] | ❌ | TS+Rust napi | hidden `--base-url` (fragile) | Yes |
| **Amp** (Sourcegraph) | **Proprietary** [E] | ❌ | TS via Bun | **No** (BYOK removed, backend-locked) | Yes |

Key reusable-design notes:
- **aider** is user-driven (request/response), not an autonomous loop; model returns **text edits** → no function-calling needed from the backend. Lowest bar for a plain llamafile server. Its **repo map** (PageRank dependency graph for context selection) is the most worth-stealing *idea* (re-implement, don't port Python).
- **opencode**'s loop is `Vercel AI SDK streamText()` — JS-bound; porting = reimplementing the loop anyway.
- **Goose** (Rust) and **Codex CLI** (Rust) are the closest neighbors to our C/C++, both Apache, both with clean local-model config.
- **Claude Code / Cursor CLI / Amp** = proprietary all-rights-reserved binaries → **do not touch the code**; they are only relevant as *clients to point at llamafile* (§C7).

## B5. Decision — build vs vendor vs backend

**Recommendation: HYBRID, weighted to BUILD + BACKEND. Do not vendor an agent codebase.**

- **(c) BACKEND — already true, ship it as a first-class story.** Every OpenAI-compatible tool (aider, opencode, Cline, Continue, Goose, Codex CLI) works against llamafile's existing `/v1/chat/completions` today (§C7-i), and Claude Code works against the already-vendored `/v1/messages` (§C7-ii). This is the **highest-leverage, lowest-effort** win: document it, fix the gaps, done.
- **(a) BUILD a thin server-side agentic loop + reuse the TUI.** The loop is ~200 lines (§C6) and the tools + TUI + Anthropic/OpenAI plumbing already exist. Building it (vs vendoring) keeps everything in-process, single-binary, cosmocc-clean, and avoids dragging a JS/Rust runtime into the APE. **This is the right call precisely because the pieces already exist** — vendoring would add more surface than it removes.
- **(b) VENDOR — no.** The permissive agents are JS (opencode/Cline/Continue/aider) or Rust (Goose/Codex), none of which compile into our cosmocc C++ server more cheaply than our own loop; the good ones (CC/Cursor/Amp) are proprietary. **Steal *ideas* (aider's repo-map, opencode's tool-output pruning thresholds, Codex's sandbox model), not code.**

---

# PART C — llamafile as opencode-like agent / Claude-Code drop-in

## C6. `llamafile --agent` (the local opencode/claude)

**Goal:** a CLI that runs the server-side agentic loop against the built-in coding tools + MCP, in the user's cwd — the local-model equivalent of `opencode`/`claude`.

**What already exists** [E]: the 8 coding tools (`server-tools.cpp:733-740`), MCP host (per `MCP-WIKIPEDIA-DESIGN.md` §5), native tool-calling (jinja render + chat-peg autoparser → `finish_reason:tool_calls`), and a **TUI** with a clean backend abstraction: `ChatBackend` (`chatbot_backend.h`) with `DirectBackend` (in-process `llama_decode`, `--chat` mode) and `ApiBackend` (HTTP to `/v1/chat/completions`). The REPL is `chatbot_repl.cpp`; entry `chatbot_main.cpp` (`main` for direct, `api_main` for HTTP).

**What's missing:** the loop that, on `finish_reason:tool_calls`, dispatches to the tool registry / MCP, appends `role:tool` results, and re-prompts until done. Today that lives in the browser (`tools/ui agenticStore`, `MCP-WIKIPEDIA-DESIGN.md` §2).

**Minimal implementation (decisive):**
1. **Add `runAgentLoop()` in a new `llamafile/`-owned file** (keep upstream `tools/server/` clean per the repo's standing rule). It is engine-agnostic over `ChatBackend`:
   - render messages + tool schemas → `backend.complete()` → if no tool_calls, return; else execute each tool_call via the `server_tools` registry (in-process) or MCP host, append `role:tool` results, loop.
   - Reuse `server_tools::invoke(name, params)` (`server-tools.cpp:811`) directly in the DirectBackend path (no HTTP hop); use `POST /tools` in the ApiBackend path.
2. **Add an `AgentBackend` (or extend the REPL)** that wraps `runAgentLoop` so the existing TUI renders the streaming turns, tool calls, and results — i.e., **reuse `chatbot_repl.cpp` + `chatbot_backend.h` verbatim**, just feed it the agent loop instead of a single completion.
3. **CLI surface:** `llamafile --agent` (cwd-scoped coding agent, gated like the existing `--tools`/`--agent` tool-enable per `MCP-WIKIPEDIA-DESIGN.md` §2). Add compaction (§A2) and the frozen-prefix discipline (§A3) inside the loop.
4. **Context features:** read `AGENTS.md` at start; maintain the two-tier scratch file; trigger compaction at ~70% of `n_ctx`.

This is the **server-side `/v1/agentic/chat`** already named as future work in `MCP-WIKIPEDIA-DESIGN.md` §5 / phase 5 — the CLI is that loop + the existing TUI. **Estimated net new code: a ~200-line loop + a thin backend wrapper + arg wiring.** Sketch in §C9.

## C7. Drop-in replacement — what backends need, and how close we are

### (i) Point opencode/aider/Cline/Codex at llamafile's `/v1`
**Works today** [E]: `/v1/chat/completions` (`server.cpp:215`), `/v1/models` (`:213`), `/v1/completions`, streaming SSE, **native OpenAI tool-calling** (jinja + autoparser → `tool_calls`, `finish_reason:tool_calls`). Per-request `lora`, `cache_prompt`. So:
- **aider:** `OPENAI_API_BASE=http://localhost:PORT/v1 --model openai/<name>` — works (it doesn't even need tool-calling).
- **opencode / Cline / Continue / Codex CLI:** point their OpenAI-compatible provider block at the base URL. Requirements they expect: function-calling (✅ with `--jinja`), SSE streaming (✅), `GET /v1/models` (✅ `:213`).
- **Gaps to verify/handle:** (1) run with `--jinja` or tool-calling won't render; (2) **model name** must match what the client sends (or be ignored) — `/v1/models` reports the loaded model; (3) **prompt-caching headers** (OpenAI `prompt_cache_key` / Anthropic `cache_control`) are no-ops here — harmless, but clients that *require* a cache acknowledgment may warn; (4) tool-call **format robustness** depends on the model's chat template (R2 in `MCP-WIKIPEDIA-DESIGN.md` §10).

### (ii) Claude Code via the Anthropic Messages API — **largely already done** [E]
**This is the big finding.** The vendored server already implements the Anthropic surface CC needs:
- `POST /v1/messages` (`server.cpp:221`) and `POST /v1/messages/count_tokens` (`server.cpp:238`) — the latter is **effectively required** by CC (it calls it every turn).
- Anthropic streaming SSE: `to_json_anthropic_stream()` (`server-task.cpp:815`) with `message_start`/`content_block_*`/`message_delta`/`message_stop` and **`input_json_delta`** for streamed tool args (`server-task.cpp:925`); `format_anthropic_sse` (`server-common.cpp:1373`).
- Anthropic→OpenAI request conversion `server_chat_convert_anthropic_to_oai` (`server-chat.cpp:325`): top-level `system`, typed content blocks, `tool_use`/`tool_result`.
- A **Claude-Code-specific billing-header normalizer** (`server-chat.cpp:303`) — strong evidence CC was actually tested against this server.

**To use it:** `ANTHROPIC_BASE_URL=http://127.0.0.1:PORT`, `ANTHROPIC_AUTH_TOKEN=dummy` (set *something* or CC may trigger login), `ANTHROPIC_MODEL=<name>`, server run with `--jinja`. This avoids the lossy JS-proxy route (claude-code-router / LiteLLM / y-router — all MIT, but JS/Python, so irrelevant when we have it native).

**Gaps to verify (concrete TODO):**
1. **Tool-calling round-trip through `/v1/messages`** end-to-end with CC (the OAI path is proven; verify the Anthropic conversion emits well-formed `tool_use` and accepts `tool_result` from CC). Re-run the `MCP-WIKIPEDIA-DESIGN.md` P2 prototype but through `/v1/messages`.
2. **Beta-field tolerance:** CC sends `anthropic-beta` headers + `context_management`/compaction fields; confirm the server ignores unknown fields (escape hatch on CC side: `CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS=1`).
3. **`count_tokens` fidelity:** CC budgets against it — confirm it returns a sane count, not a stub.
4. **Note the router caveat:** in multi-model router mode all these routes are *proxied* (`server.cpp:177-178`), so verify single-model direct mode serves them natively (the direct handlers exist; the proxy is only the multi-model front).

**Feasibility verdict: HIGH — CC drop-in is reachable now**, modulo the above verification. This is a flagship "use Claude Code with a fully-local model in one portable binary" story.

## C8. Security/safety for a local coding agent (brief)

`exec_shell_command` (`server-tools.cpp:369`) + `write_file` (`:425`) + `edit_file`/`apply_diff` make this a code-execution agent. Minimum bar:
- **cwd scoping / path jail — NOT present today [E].** Grep of `server-tools.cpp` finds **no `realpath`/`..`/canonicalization/root-prefix check** in the file tools — they take raw paths. **This must be added (high priority):** confine `read_file`/`write_file`/`edit_file`/`apply_diff` to the launch cwd subtree (canonicalize + reject escapes/absolute paths outside root).
- **Approval modes:** default to **ask-before-exec/write** (Claude-Code-style "plan vs auto-accept-edits vs full-auto"); a `--yolo`/`--auto` opt-out for trusted loops. Gate `exec_shell_command` separately from file writes.
- **Sandboxing:** `exec_shell_command` **already has a timeout** (default 10s, capped at `SERVER_TOOL_EXEC_SHELL_COMMAND_MAX_TIMEOUT`, `server-tools.cpp:386,397-400`) [E]. Add: mirror Codex CLI's OS sandbox (macOS Seatbelt / Linux Landlock / Windows job tokens) where available; at minimum a denylist + no-network default.
- **Tool enablement is already gated** by `--tools`/`--agent` (`MCP-WIKIPEDIA-DESIGN.md` §2) — keep destructive tools off by default; require explicit enable.
- **MCP servers** run as subprocesses — treat their tools as untrusted; same approval gate.

(Defer a full sandbox design; ship cwd-jail + approval modes first — that covers the 80% risk.)

## C9. Prototype sketch (server-side agentic loop — NOT built)

Pseudocode for `runAgentLoop` (the ~200-line core; engine-agnostic over `ChatBackend`):

```cpp
// llamafile/agent_loop.cpp  (new, llamafile-owned)
std::string runAgentLoop(ChatBackend& be, std::vector<common_chat_msg>& msgs,
                         server_tools& tools, McpHost* mcp, AgentPolicy pol) {
  for (int turn = 0; turn < pol.max_turns; ++turn) {
    // 1. compaction check (A2/A3): elide old tool bodies; if used > 0.70*ctx, summarize-and-restart
    maybe_compact(msgs, be.context_used(), be.context_max(), pol);

    // 2. one model turn (streams to TUI via on_token)
    std::string out = be.complete(msgs, on_token);          // ChatBackend (Direct or Api)
    auto calls = parse_tool_calls(out);                     // from finish_reason:tool_calls

    if (calls.empty()) return out;                          // done

    msgs.push_back(assistant_msg_with_tool_calls(out, calls));
    for (auto& c : calls) {
      if (pol.needs_approval(c)) if (!ask_user(c)) { append_denied(msgs,c); continue; }
      json result = tools.has(c.name) ? tools.invoke(c.name, c.args)   // server-tools.cpp:811
                                      : mcp->call(c.name, c.args);     // MCP host
      msgs.push_back(tool_result_msg(c.id, truncate(result, pol.tool_out_cap)));
    }
  }
  return "[agent: max turns reached]";
}
```
Anthropic shim is **not** needed as a prototype — it already exists (§C7-ii); the only prototype worth writing is the loop above wired into `chatbot_repl.cpp`, plus a verification harness that drives Claude Code at `/v1/messages` (extend `MCP-WIKIPEDIA-DESIGN.md` P2).

---

## Appendix — open items to verify (cheap, before building)
1. **RESOLVED [E]:** file tools have **no cwd-jail** (must add, §C8); `exec_shell_command` **has a timeout** (default 10s, capped).
2. Drive Claude Code at `/v1/messages` single-model mode end-to-end with a tool (§C7-ii TODO 1–4).
3. Add OS sandbox hook for `exec_shell_command` (timeout already present).
4. Measure role-prompt prefill time to decide if the §A4 KV bundle is worth shipping vs runtime warming.
5. Decide adapter strategy (§A5): prompt-only role steering first; LoRA only if insufficient — and remember different-adapter slots don't co-batch.
```

---

## Addendum — Claude Code drop-in VERIFIED live (2026-06-29)

Tested the vendored Anthropic Messages API directly against `llamafile --server --jinja` (Qwen3.6-35B-A3B):
`POST /v1/messages` with an **Anthropic-format** request (`messages` + `tools[].input_schema`) returned a fully **Anthropic-format** response:
- `type:"message"`, `role:"assistant"`, `stop_reason:"tool_use"`
- `content` blocks = `["thinking", "tool_use"]`, with `tool_use` = `search_wikipedia({"query":"Eiffel Tower"})`.

This confirms the headline finding at the wire level: **Claude Code can use llamafile as a backend** via `ANTHROPIC_BASE_URL=http://host:port` + a dummy `ANTHROPIC_AUTH_TOKEN` + `--jinja`, including native tool-calling (and thinking blocks). Remaining for a full CC session: verify the multi-turn `tool_result` round-trip through `/v1/messages` and beta-header tolerance — but the core protocol + tool_use path works today. CC-dropin is no longer "~80% built" speculation; the tool round-trip's first half is empirically validated.

**UPDATE — turn-2 verified too:** fed an Anthropic `tool_result` block back via `/v1/messages` → `stop_reason:"end_turn"`, grounded answer ("…the Eiffel Tower is 330 meters (1,083 feet) tall…"). The **full multi-turn Anthropic tool round-trip works through llamafile** — CC drop-in validated end-to-end at the protocol level (remaining: real CC-CLI session smoke + beta-header tolerance).
