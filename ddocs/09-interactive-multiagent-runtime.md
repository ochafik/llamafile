# llamafile: interactive multi-agent runtime — design (2026-06-29)

> Turns the working multi-agent *core* (sub-agents-as-tools, ddoc 02) into an **interactive agent runtime**. Requirements locked with the user:
> 1. **Responsiveness = async + live streaming** (orchestrator never blocks on a sub-agent).
> 2. **Comms = full message-passing mesh** (agents address + message each other; mailboxes).
> 3. **Code interpreter = headless CDP** (server-side JS via the browser tool).
> 4. **Sessions = persisted, pausable/resumable** (list/pause/resume, survive restart via KV save/restore).
> 5. (default) **Traces** = structured events on SSE + per-session JSONL + UI timeline.
> 6. (default) **Scheduling** = `wait`/`poll_until`/`schedule` tools + wake-on-event.
>
> Everything extends built pieces — no rewrite. llamafile-owned; llama.cpp pristine.

## 1. The core abstraction: an in-process agent runtime
A **Runtime** owns N **Agents**. Each Agent = `{id, name, role/system-prompt, tool-allowlist, conversation, own llama_context (KV), mailbox (queue), status}`. A **Router** delivers messages between agents (and to/from the user/UI). A **Scheduler** runs an agent's turn when it has input (a user/agent message, a tool result, a timer fire) — agents are otherwise idle (hold **no** generation slot when waiting, per the ddoc 02 slot-occupancy fact). This is the SendMessage/mailbox model the host itself uses, realized in C++.

```
                 ┌─────────── Runtime (per Session) ───────────┐
   user/UI ⇄ SSE │  Router (addressed msgs)   Scheduler (jobs)  │
                 │   ┌─────────┐  ┌──────────┐  ┌──────────┐     │
                 │   │orchestr.│  │researcher│  │ verifier │ …   │  each: mailbox + llama_context(KV) + tools
                 │   └────┬────┘  └────┬─────┘  └────┬─────┘     │
                 │        └─ send_message / spawn_agent ─┘       │
                 └──────────────────────────────────────────────┘
   tools (registry): wiki/wikidata/browser/code_run/wait/poll/schedule + send_message/spawn_agent/await
   persistence: per-session dir { agents.json, <agent>.kv (llama_state_seq_save_file), trace.jsonl }
```

Agents run their turns on **8 MiB-stack pthreads** (the established cosmocc pattern; no `std::thread`). The Scheduler is a fixed worker pool sized to `n_parallel` (slots), so concurrency = the continuous-batching budget; surplus runnable agents queue.

## 2. Responsiveness — async + live streaming
- **`spawn_agent(role, task)`** and **`send_message(to, content)`** are **non-blocking** tools: they enqueue work + return immediately (id / ack). The orchestrator keeps its turn going and stays available to the user.
- A sub-agent runs as a **job** (pthread): its `agent_loop` (ddoc 02) streams events to the session SSE as it goes; on completion it `send_message`s its result to the requester's mailbox.
- The orchestrator collects results via its mailbox: either reactively (a result-message wakes it) or by an explicit **`await(ids, timeout)`** tool when it needs them to proceed. So "fan out 3 researchers, keep chatting, synthesize when they report" is the default flow.
- **SSE multiplexing:** one `/agent/events` (or `/session/<id>/events`) stream carries every agent's events tagged by `agent_id` (+ `parent_id`), so the UI can render concurrent lanes live.

## 3. Comms — full message-passing mesh
- New tools: **`send_message(to, content)`**, **`spawn_agent(role|prompt, name?, tools?)`→id**, **`await(ids?, timeout?)`**, **`list_agents()`**, **`broadcast(content)`** (optional). Agents addressed by id or name.
- Each agent has a **mailbox**; the Scheduler runs an agent's turn when its mailbox is non-empty (messages injected as `role:user`/system turns with a `from:` header) or a timer/tool-result arrives.
- **Loop/runaway guards** (critical for a mesh): per-agent turn cap, max live agents per session, max spawn depth, a global token budget, and cycle/storm detection (rate-limit a→b→a). Agent-as-tool (`delegate_to_*`) remains as sugar over `spawn_agent`+`await` for the simple hierarchical case.

## 4. Code interpreter — headless CDP
- **`code_run_js(code, timeout)`** tool: reuses the browser-tool CDP client to `Runtime.evaluate` JS in a dedicated headless target (`about:blank`), returns result + console + thrown errors. Server-side, sandboxed in the browser JS engine; works with no user browser (LAUNCH headless). Optional `code_render_html(html)` → screenshot for plots/charts.
- Guardrails: per-eval timeout, no network unless allowed (CDP can block), output cap, the existing `--browser` opt-in. (A future `sandboxed-exec` Python tier is out of scope per the choice.)

## 5. Sessions — persisted, pausable/resumable
- A **Session** = a Runtime instance with an id. Endpoints: `POST /session` (create), `GET /sessions` (list + status/agent-count/last-activity), `GET /session/<id>` (state), `POST /session/<id>/pause|resume|stop`, `GET /session/<id>/events` (SSE).
- **Pause** = stop scheduling; finish/checkpoint in-flight turns; snapshot. **Resume** = reload + reschedule. **Persistence** per session dir: `agents.json` (ids/roles/conversations/tool-allowlists/status) + `<agent>.kv` via **`llama_state_seq_save_file`/`load_file`** (the KV mechanics from ddoc 05's bundled-KV research) so a resumed agent skips re-prefill. Survives restart. (Fingerprint the KV by model+settings, per ddoc 05; fall back to re-prefill from the saved conversation if the model changed.)
- v1 cap: one *active* Runtime decoding at a time per model/backend (the webcam-agent's dedicated-context note); paused sessions are just disk state.

## 6. Traces / observability (default)
- Every agent turn emits structured **trace events**: `{ts, session, agent_id, parent_id, type: turn|tool_call|tool_result|message|spawn|await|token?, tool, dur_ms, tokens}`. Streamed on the session SSE **and** appended to `trace.jsonl` in the session dir.
- UI: a live **agent tree/timeline** (lanes per agent, nested spawns, tool calls inline). `GET /session/<id>/trace` serves the JSONL. OTel export = later.

## 7. Scheduling (default)
Tools (suspend without holding a slot; resume via the Scheduler/timer→mailbox):
- **`wait(seconds)`** — sleep this agent.
- **`poll_until(tool, args, predicate, interval, timeout)`** — re-run a tool until a JS/简 predicate holds or timeout.
- **`schedule(delay|cron, task|message)`** — fire a task/message later (a timer thread → Router). Wake-on-event = a message to the agent's mailbox.

## 8. New surface summary
- **Tools** (registry, available to agents per allowlist): `spawn_agent`, `send_message`, `await`, `list_agents`, `code_run_js`, `wait`, `poll_until`, `schedule`.
- **Endpoints** (llamafile-owned, registered like the webcam `/agent/*`): `/session` CRUD + `pause|resume|stop`, `/sessions`, `/session/<id>/events` (SSE), `/session/<id>/trace`.
- **Files:** `llamafile/agent_runtime.{h,cpp}` (Runtime+Agent+Router+Scheduler+mailboxes+jobs), `llamafile/agent_session.{h,cpp}` (sessions+persistence+KV save/restore), extend `agent_loop` (event emission + mailbox injection + non-blocking spawn), `mcp_server`/registry (the new tools), a `code_interp` tool (CDP), trace plumbing, UI.

## 9. Phased implementation (each phase builds + tests green)
1. **Runtime + mailboxes + async jobs + SSE mux + traces** — turn `agent_loop` into scheduled mailboxed agents; `spawn_agent`/`send_message`/`await` (non-blocking); structured trace events on SSE + JSONL. Gate: orchestrator fans out 2 researchers async, stays responsive, collects results; trace shows the tree. (unit: router/mailbox/scheduler; integration: a scripted mesh run.)
2. **Scheduling tools** — `wait`/`poll_until`/`schedule` on the Scheduler/timer. Gate: an agent waits/polls without holding a slot; a scheduled message wakes it. (unit: timer→mailbox.)
3. **Code interpreter** — `code_run_js` via headless CDP (reuse browser tool). Gate: agent computes via JS, returns result. (integration, browser-gated.)
4. **Sessions + persistence** — session CRUD/list/pause/resume + KV save/restore. Gate: create→run→pause→(restart)→resume continues; `/sessions` lists status. (unit: persist/reload roundtrip; integration: pause/resume.)
5. **UI** — agent-lane/timeline view + session manager panel in the web UI (extends the embedded UI / webcam-UI pattern).

## 10. Risks
- **Mesh runaway / message storms** → hard caps + cycle/rate limits + global budget (R: highest).
- **One-decoder-at-a-time** on a single model/backend → the Scheduler serializes decode; true parallel agents share the batch (continuous batching), not separate decoders → keep the slot-pool = `n_parallel`; document the latency model.
- **KV-state resume fragility** (model/settings must match) → fingerprint + fall back to conversation re-prefill.
- **Hybrid-model KV save/restore** (Qwen3.6 recurrent state) → verify `llama_state_seq_save_file` captures recurrent state; else resume via re-prefill.
- **CDP code-interp security** → sandbox/timeouts/opt-in (reuse browser guardrails).
- **CPU speed** → async hides latency but throughput is still CPU-bound; GPU/Metal is the lever (already works).

---

## 11. Agentic-flow EVAL harness (test coverage — reliability + token cost)
Beyond the hermetic unit/integration tests, add a **behavioral eval** that runs simple objectives many times and measures reliability + the multi-agent token multiplier (the ddoc 03 concern). Model-gated (needs the model + runtime) → committed + runnable, NOT in `make check`; produces a report.

`tests/eval/agentic_flows.py` (runs against a live `--server` runtime):
- **Objectives** (simple, deterministically gradable):
  | # | Objective | Flow | Grade (pass if) |
  |---|---|---|---|
  | E1 | "Height of the Eiffel Tower?" | single-agent + wiki | answer contains 330/324/300 m |
  | E2 | "What country is the Eiffel Tower in? (use Wikidata)" | single + wikidata | contains "France" |
  | E3 | "Compare Eiffel Tower vs Statue of Liberty height; which is taller?" | orchestrator + 2 researchers (parallel) | contains "Eiffel" AND ("taller"/the right ordering) |
  | E4 | a claim to verify (one true, one false) | researcher + verifier | correct verdict |
- **Protocol:** each objective × **N=10 attempts**. Per attempt record: pass/fail (grader = substring/regex; optional LLM-judge fallback), and **total tokens across ALL agents** (sum `tokens_prompt+tokens_completion` over every agent_id in that run's trace.jsonl / the session total field), wall-clock, #agents, #turns.
- **Report (per objective):** success **X/10**, token stats (mean/median/min/max across attempts), turns/agents; and the **multi-agent multiplier** = E3 tokens ÷ a single-agent baseline answering the same. Flag objectives below a success threshold and the cost of the mesh.
- **Why:** turns "it worked once" into measured reliability + a token budget per objective; catches regressions in flow quality/cost; quantifies whether multi-agent earns its token cost (ddoc 03 §local-model: fan-out multiplies the same model's error rate + tokens).
- Requires (Phase 1): per-agent token usage in trace.jsonl + a session token total + a final-answer marker. A `tests/eval/run.sh` boots the server, runs the matrix, prints the table, tears down.
