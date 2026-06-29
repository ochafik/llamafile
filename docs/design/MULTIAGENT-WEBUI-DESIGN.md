# llamafile: multi-agent orchestration in the web UI — design (2026-06-29)

> One llamafile server, continuous batching, a tool-enabled **orchestrator** that delegates to
> **ideators / researchers / verifiers** — all served by the same model. Grounded in the existing
> agentic loop (`tools/ui` `agenticStore`), the `server_tool` registry (`server-tools.cpp`,
> `--tools`/`--agent`), and the MCP/Wikipedia design (`docs/design/MCP-WIKIPEDIA-DESIGN.md`).
> Status: **design + runnable prototype** (`scratchpad/multiagent/orchestrate.py`, `--mock` validated).

## 0. TL;DR (the decisions)
- **Topology: `agent-as-tool`, run CLIENT-SIDE first** by extending `agenticStore`. Each sub-agent
  (ideator/researcher/verifier) is exposed to the orchestrator **as a tool**; calling it runs that
  sub-agent's own agentic loop against the same `/v1/chat/completions` and returns its final text.
  This is a *recursion* of the loop that already exists — minimal new surface. **Migrate** the
  recursion into a llamafile-owned `/v1/agentic` later (the same migration the MCP design already
  plans for non-browser clients, §5 there).
- **Reject** the flat blackboard/message-bus alternative as the default (§1.4): it needs a new
  scheduler, new state store, and new UI; agent-as-tool reuses `tool_calls` end to end.
- **Concurrency: `-np 8` is the right default** for Qwen3.6-35B-A3B Q4_K_M on 96 GB (math in §2).
  The load-bearing fact: **an agent occupies a slot only while it is generating tokens.** Between
  turns — i.e. while waiting on a sub-agent or a tool — its HTTP request has already returned with
  `finish_reason:"tool_calls"`, so it holds **zero** slots. That removes the classic hold-and-wait
  deadlock and means *slot demand = number of agents generating simultaneously = fan-out width*.
- **A3B (~3 B active params/token) is batching-friendly**: batched decode amortizes weight reads
  across slots; throughput scales ~linearly to ~b=8 then sublinear (consistent with measured
  MoE-A3B batch scaling). 8 slots is the interactive sweet spot; 12–16 with Q8 KV if maximizing
  throughput.

---

## 1. Topology & where it lives

### 1.1 What exists today (cited)
The web UI **already runs a full agentic loop, client-side**, one loop per conversation:
- `agenticStore.executeAgenticLoop()` — `agentic.svelte.ts:476` — the `while(true)` turn loop:
  calls `ChatService.sendMessage(..., { tools })` with `stream:true`
  (`agentic.svelte.ts:563`), reads `tool_calls`, appends `role:"tool"` results, re-POSTs until a
  turn has no tool calls (`agentic.svelte.ts:672`).
- Tool set assembled in `toolsStore.getEnabledToolsForLLM()` — `tools.svelte.ts:239` — union of
  **builtin** (`POST /tools`), **frontend** (JS sandbox), **MCP**, and **custom** tools.
- Tool dispatch by source — `agentic.svelte.ts:782` — `BUILTIN → ToolsService.executeTool` (`POST
  /tools`, `tools.service.ts:24`), `FRONTEND → SandboxService`, else `MCP → mcpStore.executeTool`.
- Already-built control affordances we reuse for free: **turn cap + continue prompt**
  (`agentic.svelte.ts:519`), **per-tool permission gating** (`requestPermission`,
  `agentic.svelte.ts:303`), **steering messages** mid-flow (`agentic.svelte.ts:659`), per-turn
  timings/stats (`agenticTimings`).

Today the inner `for` over `normalizedCalls` executes tool calls **sequentially**
(`agentic.svelte.ts:724`). That is the one thing we change for concurrency (§2.4).

### 1.2 Recommendation: agent-as-tool, recursing the existing loop
Each sub-agent is **a tool definition injected into the orchestrator's `tools` array**:

```
delegate_to_ideator({task})       -> runs an ideator   loop, returns its final text
delegate_to_researcher({task})    -> runs a researcher loop (wiki/browser/fetch), returns summary
delegate_to_verifier({claims})    -> runs a verifier   loop, returns PASS/FAIL + evidence
answer_user({answer})             -> terminates the orchestrator, renders to the user
```

When the orchestrator emits `delegate_to_researcher`, the dispatcher does **not** hit `/tools` or
MCP — it **calls `executeAgenticLoop` again** with that role's system prompt + tool allowlist, on a
fresh sub-conversation, and returns the sub-agent's final assistant text as the `role:"tool"`
result. This is the same loop, nested. Concretely it's a new branch in the dispatch switch at
`agentic.svelte.ts:782`:

```
else if (toolName.startsWith('delegate_to_')) {
    const role = toolName.slice('delegate_to_'.length);
    result = await this.runSubAgent(role, args, { signal, depth: depth + 1 });
}
```

`runSubAgent` builds `[{role:system, content: ROLE_PROMPT[role]}, {role:user, content: task}]`,
sets `tools = allowlistFor(role)`, and calls the **same** `executeAgenticLoop` (parameterized by a
`depth` guard and a sub-conversation id). The orchestrator is just a sub-agent whose tools happen to
include the `delegate_*` family + `answer_user`.

Why this is the right call:
- **Minimal new code.** No new protocol, no new state machine. The loop, tool registry, permission
  gating, turn caps, and timings all already exist and are reused verbatim per nesting level.
- **Uniform with the tool model.** The orchestrator sees sub-agents exactly as it sees `wiki_search`
  — a JSON schema in `tools`, a `tool_calls` request, a `role:"tool"` result. No special-casing in
  the model's prompt format.
- **Composes with continuous batching for free** (§2): N concurrent `delegate_*` calls become N
  concurrent `/v1/chat/completions` requests on one server → the batched scheduler runs them in one
  decode. No extra infra.
- **Maps onto the planned `/v1/agentic` migration.** The MCP design already plans a llamafile-owned
  server-side loop for non-browser clients (`MCP-WIKIPEDIA-DESIGN.md` §5/§8.5). The agent-as-tool
  recursion is identical whether driven by the browser or by that server loop, so we move it down a
  layer later without rethinking the topology.

### 1.3 Nesting, depth, and concurrency in one picture
```
orchestrator loop  (slot only while generating its turn)
  └ turn 0: tool_calls = [ideator, researcherA, researcherB]   ← fan-out
       │  Promise.all -> 3 concurrent sub-agent loops -> 3 concurrent /v1 requests
       ├ ideator     loop (no tools)                    [slot]
       ├ researcherA loop -> wiki_search, browser_open  [slot while generating; leaf tools need NO slot]
       └ researcherB loop -> wiki_search, web_fetch     [slot]
  └ turn 1: tool_calls = [verifier]   (orchestrator regained a slot only to emit this turn)
       └ verifier loop -> wiki_search                   [slot]
  └ turn 2: tool_calls = [answer_user]  -> stream final answer to user, done
```
Key: leaf tools (`wiki_search`, `browser_open`, `web_fetch`) are `server_tool`/MCP/frontend calls,
**not** model calls — they consume **no** slot. Only sub-**agent** turns consume slots.

### 1.4 Alternative considered: flat blackboard / message-bus
A peer mesh where agents post to a shared blackboard and a scheduler dispatches. **Rejected as
default:** it needs (a) a new scheduler + queue, (b) a new shared-state store outside the
conversation model, (c) brand-new UI to visualize a non-tree topology, and (d) a separate
termination/quiescence detector. Agent-as-tool gets termination (no tool calls → done), state (the
nested conversation), and topology (a call tree) **for free** from machinery already in
`agenticStore`. Keep the blackboard in mind only if we later need many-to-many peer negotiation
(debate/consensus); it can be layered as a `post_to_board`/`read_board` `server_tool` pair without
changing the loop.

---

## 2. Concurrency / continuous batching capacity

### 2.1 Server mechanics (cited)
`-np N` (a.k.a. `--parallel`) gives the server N slots; auto-mode picks `n_parallel = 4` with
`kv_unified = true` (`server.cpp:126-130`). `--cont-batching` (continuous batching, default on)
lets the scheduler pack tokens from different slots into one decode step. Each slot is an
independent sequence with its own KV region; with `kv_unified=true` they share one KV buffer sized
by the total `--ctx-size`, sliced across sequences.

### 2.2 KV-cache memory — the formula
```
KV_bytes_per_token = 2 (K+V) × n_layers × n_kv_heads × head_dim × bytes_per_elem
total_KV          = ctx_per_slot × n_slots × KV_bytes_per_token        (kv_unified: ctx_total × per-token)
RAM_budget        ≈ model_weights + total_KV + compute/activation buffers
```
Using **Qwen3-30B-A3B as the architecture proxy** for Qwen3.6-35B-A3B (GQA: 48 layers, 4 KV heads,
head_dim 128 — the 35B variant is in the same family; re-measure once the GGUF metadata is read):
```
KV/token (f16) = 2 × 48 × 4 × 128 × 2 B = 98,304 B ≈ 96 KiB/token
KV/token (q8_0 KV cache) ≈ 48 KiB/token
```

| ctx/slot | KV/slot f16 | KV/slot q8 |
|---|---|---|
| 8 K  | 0.75 GiB | 0.38 GiB |
| 16 K | 1.5 GiB  | 0.75 GiB |
| 32 K | 3.0 GiB  | 1.5 GiB  |

### 2.3 Budget on a 96 GB Mac (model ≈ 22 GB Q4_K_M)
Usable unified memory for Metal is ~70 % of RAM by default (raisable via
`iogpu.wired_limit_mb`), call it ~70 GB. Model 22 GB leaves ~48 GB for KV + buffers.

| config | total KV | model+KV | verdict |
|---|---|---|---|
| **-np 8 @ 32 K f16** | 24 GiB | 46 GiB | **recommended default — comfortable** |
| -np 8 @ 32 K **q8 KV** | 12 GiB | 34 GiB | lots of headroom |
| -np 16 @ 32 K f16 | 48 GiB | 70 GiB | tight; only with raised wired limit |
| **-np 16 @ 32 K q8 KV** | 24 GiB | 46 GiB | **throughput-max option** |
| -np 8 @ 16 K f16 | 12 GiB | 34 GiB | conservative / long sessions |

### 2.4 Compute side — why 8 (and not 4, not 32)
A3B activates ~3 B params/token. Decode is bandwidth-bound on the **active** weights; **batching
amortizes that read across all slots in the step**, so throughput scales ~linearly with batch up to
a compute knee, then sublinear. Measured MoE-A3B-class batch scaling (this fleet's own data): the
BW wall breaks ~12× by b≈8 while per-slot rate degrades gracefully; CPU GEMM saturates near b=8,
and Metal/GPU has more headroom. So:
- `-np 4` (the auto default) under-uses the batching win and caps fan-out at 4.
- `-np 8` hits the throughput knee and supports orchestrator + a healthy fan-out.
- `-np 16/32` only pays off for raw throughput with Q8 KV; interactive per-token latency suffers and
  KV gets expensive.

**Recommended config:**
```
llamafile --server --jinja --tools \
  -np 8 --cont-batching --ctx-size 262144   # 256K total = 32K/slot, kv_unified
  # add --cache-type-k q8_0 --cache-type-v q8_0 to drop KV ~2× and enable -np 16
```

### 2.5 Mapping agents → slots (the deadlock argument)
- **An agent holds a slot only during token generation.** While it waits on a leaf tool or a
  sub-agent, its `/v1/chat/completions` call has already returned (`finish_reason:"tool_calls"`).
  So *idle/waiting agents consume zero slots.*
- **Slot demand = number of agents generating at the same instant = fan-out width.** Parallel
  researchers each need a slot *only while decoding*; the orchestrator needs a slot *only* on the
  turns where it is producing its own tokens (plan / reconcile / answer) — **not** while its
  delegated sub-agents run.
- **No hold-and-wait deadlock.** The orchestrator is not occupying a slot while blocked on a
  sub-agent, so a sub-agent queued behind it cannot starve it. If fan-out > `-np`, the surplus
  requests simply **queue in the continuous-batching scheduler** and drain as slots free — bounded
  latency, never deadlock.
- **Client-side cap to match the server:** the harness/UI caps simultaneous in-flight sub-agent
  requests with a semaphore of size `-np` (see `Registry.sem` in the prototype). This keeps the
  scheduler queue short and latency predictable; set fan-out soft cap ~6 to leave the orchestrator
  headroom.

---

## 3. Roles & prompts

System-prompt sketches + tool allowlists (full text in the prototype `ROLE_PROMPTS`/`ROLE_TOOLS`):

| role | tools | system prompt (essence) |
|---|---|---|
| **orchestrator** | `delegate_to_ideator`, `delegate_to_researcher`, `delegate_to_verifier`, `answer_user` (+ optional `read_file`/`grep`) | "You talk to the user. Decompose; delegate (you may call several delegates in ONE turn to run them in parallel); reconcile; then `answer_user`. Never research yourself." |
| **ideator** | *(none)* | "Brainstorm angles, hypotheses, sub-questions. No external tools. Return a bulleted list." |
| **researcher** | `wiki_search`, `wiki_get_article`, `browser_open` + `browser_*` family, `web_fetch` | "Gather facts via tools; cite sources; return a short factual summary." |
| **verifier** | `wiki_search`, `web_fetch` (read-only subset) | "Adversarially re-check each claim against sources; return PASS/FAIL + evidence; flag unsupported claims." |

**Decompose → fan out → reconcile → stream:**
1. **Decompose** — orchestrator turn 0: emits `delegate_*` calls (often several at once).
2. **Fan out** — dispatcher runs them concurrently (`Promise.all` in UI / `asyncio.gather` in the
   prototype) → batched on the server.
3. **Reconcile (completeness/critic)** — orchestrator turn 1 reads sub-agent results; if gaps,
   delegate again; route key claims through `delegate_to_verifier` before answering. The orchestrator
   *is* the completeness critic; an optional dedicated `delegate_to_critic` can gate `answer_user`.
4. **Stream status** — each sub-agent's tokens + tool calls stream into its own UI lane (§5);
   `answer_user` streams the final answer to the main thread.

---

## 4. Tool access & the registry

- **One shared registry.** All roles draw from the same union assembled in
  `toolsStore.getEnabledToolsForLLM()` (`tools.svelte.ts:239`): builtin `server_tool`s (incl. the
  inlined `wiki_search`/`wiki_get_article` from the MCP-Wikipedia design), MCP-bridged tools, the
  `browser_*` family (whatever source the browser-use agent registers it as — builtin `server_tool`
  or MCP), and custom tools.
- **Per-role allowlisting** = filter that union by role. Add a small `toolsForRole(role)` selector
  beside `getEnabledToolsForLLM()` that intersects the global enabled set with `ROLE_TOOLS[role]`.
  Reuses the existing per-tool **enable/disable** keys (`toolKey`, `tools.svelte.ts:16`) and the
  **permission** store (`requestPermission`, `agentic.svelte.ts:303`) unchanged — a denied tool is
  denied for every role.
- **Sub-agent-as-tool injection.** The `delegate_*` + `answer_user` schemas are generated
  client-side and concatenated onto the orchestrator's `tools` array only (see
  `Registry.delegate_schemas()` in the prototype). They are *not* in the `server_tool` registry —
  they're virtual tools whose "execution" is a nested loop. Sub-agents do **not** receive
  `delegate_*` by default (one orchestration layer); a `--max-depth` guard allows controlled
  nesting if a researcher is later allowed to spawn its own helpers.
- **Browser-use tool** (designed by another agent): treated as ordinary leaf tools
  (`browser_open`, `browser_*`) in the researcher allowlist; no orchestration coupling. The
  prototype stubs `browser_open` so the flow is exercisable now.

---

## 5. UI/UX

Render the call **tree** as concurrent **lanes** (one collapsible panel per active agent), nested
under the orchestrator — the topology is literally the agent-as-tool call tree.
- **Minimal `agenticStore` changes:** the store already keys sessions by `conversationId`
  (`_sessions`, `agentic.svelte.ts:136`). A sub-agent = a child session whose id is
  `${parentConvId}/sub/${role}#${n}`. The existing per-session reactive state (`isRunning`,
  `currentTurn`, `streamingToolCall`, timings) then drives each lane with **no schema change** —
  `getActiveSessions()` (`agentic.svelte.ts:181`) already enumerates running sessions for a tree
  view.
- **Streaming:** each sub-agent loop already emits `onChunk`/`onReasoningChunk`/
  `onToolCallsStreaming` (`agentic.svelte.ts:569-600`); route those callbacks to the child lane.
  The orchestrator's `answer_user` content streams to the main message thread.
- **Steering:** the existing steering-message mechanism (`injectSteeringMessage`,
  `agentic.svelte.ts:255`) lets the user interrupt/redirect the **orchestrator** mid-flight;
  on steering, in-flight sub-agents are aborted via the shared `AbortSignal` (already threaded
  through `executeAgenticLoop`).
- **Status chips:** per-lane show turn count / tool calls / tokens (already in `agenticTimings`).
- **Tree/lanes over tabs:** tabs hide concurrency; lanes show all agents progressing at once, which
  is the whole point of continuous batching.

---

## 6. Failure modes & mitigations

| failure | mitigation (mostly reuses existing machinery) |
|---|---|
| **Runaway loops** | per-agent `maxTurns` + the existing continue-prompt (`agentic.svelte.ts:519`); orchestrator gets a *global* turn/agent budget across the whole tree. |
| **Cost / turn caps** | track total sub-agent spawns + tokens in the registry; hard cap (e.g. ≤ N sub-agents, ≤ M total turns) → force `answer_user`. |
| **Slot starvation** | client semaphore = `-np` bounds in-flight requests; surplus queues in the scheduler (bounded latency). |
| **Deadlock** | structurally impossible here: a blocked agent holds no slot (§2.5). The semaphore is for latency, not correctness. Keep a **depth limit** so the tree can't recurse unboundedly. |
| **Context bloat** | sub-agents return only their **final summary** to the orchestrator, not their full transcript — the nested conversation is discarded after the `role:"tool"` result. This is the big context win of agent-as-tool vs a shared mega-context. |
| **Tool errors** | already caught per-call and fed back as `role:"tool"` error text (`agentic.svelte.ts:805-812`); sub-agent failures surface to the orchestrator as a tool result it can react to (retry / different agent). |
| **Sub-agent never terminates** | inherits `maxTurns`; on cap it returns a partial summary with a `stopped: max turns` marker the orchestrator can see. |
| **Permission fatigue across N agents** | "always for this server" decision (`ToolPermissionDecision.ALWAYS_SERVER`, `agentic.svelte.ts:327`) applies registry-wide, so approving once covers all lanes. |

---

## 7. Prototype (validated)

`scratchpad/multiagent/orchestrate.py` implements the orchestrator + ideator/researcher/verifier
via agent-as-tool, with a real tool-dispatch loop (leaf tools `wiki_search`, `browser_open`,
`web_fetch` stubbed; sub-agents as `delegate_*` tools), concurrent fan-out via `asyncio.gather`, and
a `Registry` semaphore mirroring `-np`. `--mock` runs the full control flow with a deterministic
fake LLM; default mode points `--base-url` at the llamafile server and uses the identical loop.

`--mock` run (concurrency visible — 3 sub-agents all spawn at t=0.16 s, leaf tools run in parallel):
```
[  0.16s] orchestrator   turn0      calls: delegate_to_ideator, delegate_to_researcher, delegate_to_researcher
[  0.16s]     ideator#1      spawn      (slot acquired) angles on the user question
[  0.16s]     researcher#2   spawn      (slot acquired) facet A: France energy mix 2024
[  0.16s]     researcher#3   spawn      (slot acquired) facet B: Germany energy mix 2024
[  0.31s]     researcher#2   turn0      calls: wiki_search, browser_open      ← leaf tools (no slot)
[  0.31s]     researcher#3   turn0      calls: wiki_search, browser_open
[  0.51s]     researcher#2   final      Summary: key facts gathered with 2 sources cited.
[  0.66s] orchestrator   turn1      calls: delegate_to_verifier
[  1.01s]     verifier#4     final      PASS: both claims supported by wiki snippet.
[  1.16s] orchestrator   ANSWER     France's 2024 mix is nuclear-dominated; Germany leans renewables...
```

**Real run (once Qwen is up):**
```
python orchestrate.py --base-url http://localhost:8080/v1 --model qwen3.6-35b-a3b \
    --max-concurrency 8 --task "Compare the energy mix of France and Germany in 2024."
```

---

## 8. Phased implementation
1. **Client-side agent-as-tool** in `agenticStore`: `runSubAgent` recursion + `delegate_*`/
   `answer_user` virtual tools + `toolsForRole` allowlist + depth/budget guards. Gate: orchestrator
   delegates to one researcher and answers.
2. **Concurrent fan-out**: replace the sequential tool `for` (`agentic.svelte.ts:724`) with a
   `Promise.all` that preserves result ordering; add the in-flight semaphore (= `-np`). Gate: two
   researchers run on two slots simultaneously (watch server slot logs).
3. **UI lanes**: child sessions + tree view via `getActiveSessions()`. Gate: N lanes stream at once.
4. **Tune `-np`/ctx/KV** on the real model; set defaults (§2.4). Gate: measured throughput knee.
5. **Server-side `/v1/agentic`** (the MCP design's migration): move the recursion server-side for
   non-browser clients; UI becomes one of several drivers.

## 9. Top risks
- **R1** Qwen3.6-35B-A3B exact head/layer counts differ from the 30B proxy → recompute KV from GGUF
  metadata before fixing `-np`/ctx (formula in §2.2 is the tool).
- **R2** model's chat template must handle many tools + parallel `tool_calls` well — verify the
  jinja template emits multiple tool calls per turn (needed for fan-out); fall back to one-delegate-
  per-turn if not.
- **R3** Metal wired-memory limit caps total KV — raise `iogpu.wired_limit_mb` for `-np 16`, or use
  Q8 KV.
- **R4** context bloat if sub-agents return verbose transcripts — enforce summary-only returns (§6).
