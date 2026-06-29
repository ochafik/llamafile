# Multi-Agent LLM Systems — Best Practices Brief (state of the art, June 2026)

**Purpose.** Inform a concrete design: an **orchestrator + ideators + researchers + verifiers**, all tool-enabled, served by **one local model via continuous batching**. This brief distills the 2025–2026 literature into decisive, adoptable principles and anti-patterns.

**How to read citations.** Each claim is tagged **[EVIDENCE]** (directly stated/measured in a cited primary source) or **[INFERENCE]** (our synthesis/opinion extrapolating to our setup). Dates are publication dates of the cited source. Where 2026 practice diverges from 2024 assumptions, it is flagged **⚠ DIVERGENCE**.

---

## 0. Principles to adopt / anti-patterns to avoid (the decisive list)

### Adopt

1. **Default to a single strong agent; add sub-agents only to buy parallel breadth or context isolation.** Multi-agent wins almost exclusively on *read-heavy, parallelizable, breadth-first* work. It does not win on *write-heavy, tightly-coupled* work (most coding, most synthesis). [EVIDENCE: Anthropic, LangChain, Cognition all agree on this axis]
2. **One writer.** Fan out *reading/research/ideation*; reserve *synthesis and any shared-state writes* for a single agent (or a strictly serialized critic→synthesizer). Reading parallelizes; writing does not. [EVIDENCE]
3. **Context engineering is the real variable, not agent count.** The Cognition↔Anthropic "debate" resolves to: both failed at giving each agent the right context; Anthropic engineered around it, Cognition concluded it wasn't worth it. Spend your effort here. [EVIDENCE: LangChain]
4. **Pass distilled, structured results — never raw transcripts — across agent boundaries.** Typed handoff payloads (objective, output format, constraints, sources) in; compact findings + artifact references out. [EVIDENCE]
5. **Give every sub-agent an explicit contract:** objective, output schema, tool/source allowlist, and hard task boundaries. Vague delegation is the #1 cause of duplicated/conflicting work. [EVIDENCE: Anthropic]
6. **Scale effort to query complexity, and say so in the orchestrator prompt** (e.g., simple → 1 agent/3–10 tool calls; comparison → 2–4 sub-agents; complex → 10+). Otherwise the model over-spawns. [EVIDENCE: Anthropic]
7. **Verify before commit, with perspective diversity.** Use a separate critic/verifier role; check both orderings for pairwise judging; prefer a different prompt/persona (and, on frontier stacks, a different model family) for the judge than the generator. [EVIDENCE]
8. **Externalize state.** Artifacts to a filesystem/memory store with lightweight references; compaction + structured note-taking before context limits. Don't copy big outputs through the orchestrator's window. [EVIDENCE: Anthropic]
9. **Hard budgets and loop guards are non-negotiable.** Per-request token ceiling, max-turns, max-sub-agents, and a "no new information → stop" completeness check. Treat these as circuit breakers in code, not prompt suggestions. [EVIDENCE]
10. **Trace everything with OpenTelemetry GenAI semantic conventions; propagate context across agent boundaries.** Errors compound silently; log-level monitoring won't catch tool-call failures, truncation, or runaway loops. [EVIDENCE]
11. **Exploit shared-prefix KV caching on the local model.** Identical system/role prefixes across agents are nearly free to re-serve under continuous batching; design prompts so the *stable* part is a long shared prefix and the *variable* part is short and trailing. [EVIDENCE + INFERENCE]

### Avoid

- **Parallel agents that both *write* to the same artifact/decision space.** This is the Flappy-Bird failure: two correct-in-isolation outputs that don't compose. [EVIDENCE: Cognition]
- **Multi-agent for tasks needing shared context or many inter-agent dependencies** (e.g., editing one codebase). A single linear agent is more reliable. [EVIDENCE: Cognition]
- **Forwarding full conversation traces between agents** ("just share everything"). It's token-explosive and induces context rot/distraction. Distill instead. [EVIDENCE]
- **Tool overload.** Dumping every MCP tool into context can consume a large fraction of the window and *drops* tool-selection accuracy. Allowlist per role. [EVIDENCE]
- **Over-spawning** (50+ sub-agents for trivial queries; endless re-search of nonexistent sources). [EVIDENCE: Anthropic anti-patterns]
- **Treating the LLM-judge as ground truth.** Uncalibrated judges carry position/verbosity/self-enhancement bias. Calibrate against labels; keep a human spot-check on the failing 1–5%. [EVIDENCE]
- **Many weak agents over one strong agent.** Coordination overhead + error compounding often beats the parallelism gain, especially on one shared local model where they compete for the same compute. [INFERENCE, strongly supported]

**⚠ DIVERGENCE from 2024 assumptions.** In 2024 the framing was "more agents / bigger swarms = more capability," and "bigger context windows solve memory." 2026 consensus inverts both: (a) agent count is a *cost/fragility* multiplier you justify case-by-case, not a capability dial; (b) long context *degrades* ("context rot") — most production agents break well before the nominal window, so curation beats accumulation. [EVIDENCE]

---

## 1. Single-agent vs multi-agent — when each wins

**The two anchor sources, both June 2025, look contradictory but aren't.**

- **Anthropic, "How we built our multi-agent research system"** (June 2025): orchestrator-worker with Claude Opus 4 lead spawning 3–5 parallel Claude Sonnet 4 sub-agents (each own context window) plus a separate citation pass. **Claim: +90.2% over single-agent Opus 4 on their internal research eval.** Cost: agents use ~4× chat tokens; multi-agent ~15× chat tokens; **token usage alone explains ~80% of performance variance on BrowseComp.** Good fit = open-ended, breadth-first research exceeding one context window; poor fit = shared-context/heavily-dependent tasks and *most coding*. [EVIDENCE] (https://www.anthropic.com/engineering/built-multi-agent-research-system)
- **Cognition (Walden Yan), "Don't Build Multi-Agents"** (June 2025): two principles — (1) *"Share context, and share full agent traces, not just individual messages"*; (2) *"Actions carry implicit decisions, and conflicting decisions carry bad results."* The Flappy-Bird example: parallel sub-agents produce a Mario-style background + a non-game bird that don't compose; the integrator inherits the conflict. Recommendation: **single-threaded linear agent**; for very long tasks add a **compression layer** (an LLM that distills history into key decisions). Parallel multi-agent is "fragile" until agents have human-like conflict-resolution. [EVIDENCE] (https://cognition.com/blog/dont-build-multi-agents)
- **LangChain, "How and when to build multi-agent systems"** (June 2025) reconciles them: *"there is not a 'one-size-fits-all' solution."* Both teams hit the **same** problem — giving each agent the right context. **"Read actions are inherently more parallelizable than write actions."** Anthropic delegates *research (reading)* to many agents and reserves *synthesis (writing)* for one; Cognition's coding domain is write-heavy and coupled, so single-threaded wins. [EVIDENCE] (https://www.langchain.com/blog/how-and-when-to-build-multi-agent-systems)

**Decision rule (adopt).**

> Fan out a task to parallel sub-agents **iff** it is (a) decomposable into *independent* subtasks, (b) dominated by *reading/search/ideation* not coupled writing, (c) large enough that one context window would rot, and (d) valuable enough to pay ~4–15× tokens. Otherwise run a single agent with good context engineering. [INFERENCE from the three sources, which agree on each clause]

**Implication for our orchestrator + ideators + researchers + verifiers topology.**
- **Researchers**: textbook good fit — read-only, independent, breadth-first → **fan out**. [EVIDENCE-backed]
- **Ideators**: read-only generation of *candidate* options is parallelizable *if* each produces an independent candidate and a single later stage selects/merges. Do **not** let ideators co-write one artifact. [INFERENCE]
- **Verifiers**: independent read-only checks → **fan out** with perspective diversity. [INFERENCE]
- **Orchestrator + final synthesis**: **single writer.** Decomposition, reconciliation, and the final artifact stay in one agent's continuous context. [EVIDENCE: Anthropic "synthesis for a single unified agent"]

---

## 2. Orchestration patterns

| Pattern | What it is | When to use | Cost/risk |
|---|---|---|---|
| **Orchestrator-worker** (a.k.a. supervisor, agent-as-tool) | Lead agent decomposes, calls workers as tools, results return via tool-result channel | Subtasks known at plan time; one accountability point; breadth-first research/retrieval | Synchronous bottleneck (lead waits for slowest worker); token cost | [EVIDENCE: Anthropic; OpenAI Agents SDK; LangGraph supervisor] |
| **Handoffs** (decentralized, OpenAI Swarm/Agents SDK) | Agent transfers *control + context* one-way to a specialist | Dynamic routing where you can't know the specialist until the conversation unfolds (domain routing, support triage) | Limited flexibility, higher dev overhead; control can scatter | [EVIDENCE] |
| **Planner-executor** | Explicit plan produced, then executed step-wise | Tasks with a stable decomposable plan | Plan staleness; replanning cost | [EVIDENCE] |
| **Blackboard / shared structured state** | Agents read/write a typed shared context object; orchestrator passes only relevant fields | Most token-efficient coordination; what frameworks recommend | Needs concurrency control on writes (mutex/queue semantics) | [EVIDENCE] |
| **Map-reduce / fan-out-fan-in (DAG)** | Coordinator distributes, workers run concurrently, results aggregated | Independent parallel subtasks with a clean merge step | Race conditions, unbounded fan-out vs rate limits, dedup needed | [EVIDENCE] |

**Agent-as-tool vs handoffs (concrete guidance).** OpenAI's docs and LangGraph both *explicitly separate* these. Use **agent-as-tool** when the orchestrator must stay in control and reconcile results (our case). Use **handoffs** for terminal routing to a specialist that will own the rest of the turn. [EVIDENCE]

**Task decomposition + reconciliation (best practice).**
- Decompose with explicit, non-overlapping boundaries to avoid duplicated work. [EVIDENCE: Anthropic]
- Reconcile with a **synthesizer** (single writer) plus an optional **adjudication/critic pass** that resolves conflicts and **deduplicates near-identical outputs** via similarity checks. [EVIDENCE]
- **For our topology:** orchestrator (agent-as-tool calling researchers/ideators/verifiers) + blackboard for distilled findings + single synthesizer + completeness critic. [INFERENCE]

---

## 3. Context engineering

This is the highest-leverage area (LangChain's whole point). Sources: Anthropic "Effective context engineering for AI agents" (Sept 2025); the "context rot" literature (Chroma's report and follow-ons, 2025).

- **Context rot is real and measurable.** Output quality degrades as input length grows; degradation modes include **poisoning** (a hallucination repeated every step), **distraction** (relying on accumulated history instead of synthesizing fresh plans), **confusion** (irrelevant tokens hurting quality), and **clash** (contradictory info). ⚠ Most production agents break before ~130K tokens despite 200K+ windows. [EVIDENCE] (https://www.morphllm.com/context-rot)
- **Isolation by sub-agent.** Keep detailed search/tool context *inside* the sub-agent; the lead receives only synthesized results. Clear separation of concerns. [EVIDENCE: Anthropic context-engineering post] (https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents)
- **Pass distilled, structured results, not transcripts.** This is the operational core of the Cognition↔Anthropic reconciliation: Cognition is right that *information* (decisions) must propagate; Anthropic is right that it should be *distilled*, not raw. A typed handoff payload + a compression step satisfies both. [INFERENCE, grounded in both]
- **Compaction.** Near the window limit, summarize and reinitialize a fresh window with the summary. The art is *what to keep*; over-aggressive compaction silently drops context whose importance surfaces later. [EVIDENCE: Anthropic]
- **Structured note-taking.** Agent writes to external memory (e.g., `NOTES.md`/memory tool) and pulls back as needed — persistent state, minimal token overhead. [EVIDENCE]
- **Avoid decision fragmentation.** When two agents make *implicit* decisions that conflict, the integrator inherits the conflict. Mitigations: a single writer; explicit shared decisions on the blackboard; have ideators emit *candidates with stated assumptions*, not committed artifacts. [EVIDENCE: Cognition]

**For our system.** Researcher/ideator/verifier outputs must be **structured payloads** (claim, evidence, confidence, source refs), capped in size. The orchestrator's window holds *the plan + distilled findings + the artifact under construction* — nothing else. [INFERENCE]

---

## 4. Verification

- **Separate the critic from the generator.** A dedicated critic/verifier checks completeness, internal consistency, and constraint adherence; producer refines until a threshold, bounded by a max iteration count. [EVIDENCE]
- **Perspective diversity beats more-of-the-same.** Different prompt/persona/role for the judge; on frontier stacks, **a different model family** to cut **self-enhancement bias (~5–7% boost when judging own family)**. On our single-local-model setup, achieve diversity via *different system prompts/personas and few-shot rubrics*, since model family is fixed. [EVIDENCE for the bias; INFERENCE for the single-model workaround]
- **Mitigate position bias.** Pairwise judging is order-sensitive (one source cites ~40% GPT-4 inconsistency); evaluate both (A,B) and (B,A) and require agreement. [EVIDENCE]
- **Calibrate the judge.** Three production properties: **reproducibility** (same input → same score within tolerance), **calibration** (correlates with human labels on your data), **bias control** (position/verbosity/family measured & mitigated). 2026 production pattern = hybrid: deterministic metrics where measurable, LLM-judge for reasoning-heavy checks, human review of the failing 1–5%. [EVIDENCE] (https://futureagi.com/blog/llm-as-a-judge/)
- **Self-consistency / voting** improves reliability on tasks with a checkable/convergent answer; confidence-weighting helps. Less useful for open-ended generation. [EVIDENCE]
- **How many verifiers?** Verification pays most when (a) generation is cheap relative to the cost of a wrong commit, and (b) the check is *independent* of the generator. Start with **1 critic + 1 (optional) adversarial verifier with a different rubric**; add voting (3–5) only for high-stakes, checkable claims. [INFERENCE, grounded in the verification-dynamics literature]

**For our system.** "Verify before commit" loop: synthesizer produces draft → critic (completeness) + verifier (factual/constraint, different rubric) → if fail, targeted revision (bounded iterations) → commit. [INFERENCE]

---

## 5. Parallelization discipline

- **Fan out read-only/independent work; serialize shared writes.** Race conditions on shared state, token-budget explosion, unbounded fan-out against rate-limited tools, and coordination breakdowns cause a large share of production multi-agent outages. The discipline is literally classic concurrency control (mutexes, queues, barriers). [EVIDENCE]
- **Cap concurrency.** "Disciplined governance of concurrency boundaries, resource ownership, and termination conditions" — not just the ability to fan out. ⚠ On one shared local model this is doubly important: every parallel agent competes for the *same* GPU batch (see §8). [EVIDENCE + INFERENCE]
- **Avoid redundant work two ways:** (1) non-overlapping task boundaries up front; (2) dedup near-identical outputs by similarity + an adjudication pass. [EVIDENCE]
- **Loop-until-dry / completeness-critic.** Keep iterating *only while new information is being found*; a completeness critic decides "done." Pair with a hard max-iteration cap so "loop until dry" can't run away. [EVIDENCE for critic loops; INFERENCE for the explicit dry-stop]
- **Synchronous bottleneck is the default cost.** The lead blocks on the slowest worker and can't steer mid-flight. Async helps throughput but adds coordination complexity — adopt async only once the sync version is correct. [EVIDENCE: Anthropic]

---

## 6. Tool design for agents

- **Clear schemas matter as much as prompts.** Poor tool descriptions send agents "down completely wrong paths"; one Anthropic tool redesign cut task-completion time **40%**. [EVIDENCE]
- **Token-efficient results.** Return compact, structured results; write large outputs to the filesystem and pass references, not blobs. [EVIDENCE]
- **Allowlist tools per role (default-deny).** Tool overload is a top-tier 2026 failure: bloated toolsets can consume a large fraction of the window before any work and **drop tool-selection accuracy ~3×**; generic MCP servers expose 40 functions when an agent needs 3. Give each role (ideator/researcher/verifier) only its tools. [EVIDENCE] (https://eclipsesource.com/blogs/2026/01/22/mcp-context-overload/)
- **MCP norms (2026):** least-privilege access control, review new tools before production, beware tool-poisoning. **Code-execution / "code mode"** (let the model call tools via code instead of inlining all tool metadata) is now recommended for 3+ MCP servers; reported **60–80% token reduction** with load-balancing + semantic caching + code mode. Some teams drop MCP for direct CLI/API where token cost dominates (CLI ~200 tokens/command vs tens of thousands for equivalent MCP). [EVIDENCE] (https://www.anthropic.com/engineering/code-execution-with-mcp)
- **Surface errors, don't swallow them.** Let agents see tool failures and adapt; resume-from-checkpoint rather than full restart on partial failure. [EVIDENCE: Anthropic]

**For our system.** Define a small, role-scoped toolset per agent; structured/compact tool results; artifacts to disk + refs; consider code-mode if we wire many local tools. [INFERENCE]

---

## 7. Reliability & cost

- **Errors compound across turns.** When one agent hallucinates or a tool times out, the error cascades silently; standard APM can't see it. Most incidents are tool-call failures, context truncation, and runaway loops — *not* model errors. [EVIDENCE]
- **Hard budgets as circuit breakers (in code):** per-request token ceiling, max turns, max sub-agents, per-sub-agent caps. Independent of alerting. [EVIDENCE]
- **Runaway-loop guards:** detect repeated identical tool calls / no-progress and break. [EVIDENCE]
- **Observability:** OpenTelemetry GenAI semantic conventions for vendor-neutral traces; **propagate trace context across agent/handoff boundaries** so the span tree spans the whole workflow; attribute cost per run/sub-agent/tool. High-level decision-pattern observability *plus* full production tracing (Anthropic monitors interaction structure without reading conversation contents for privacy). [EVIDENCE]
- **Determinism:** runs are non-deterministic even with identical prompts; don't expect bit-reproducibility. Use checkpointing + rainbow/rolling deploys so code updates don't break long-running agents. For *evals*, fix seeds/temperature where the serving stack allows and rely on aggregate metrics. [EVIDENCE + INFERENCE]
- **Evaluation harness:** start with ~20 representative queries (large effect sizes, 30%→80%, show up fast); LLM-as-judge with a rubric (factual accuracy, citation accuracy, completeness, source quality, tool efficiency) → 0–1 score + pass/fail; keep human eval for hallucinations and subtle biases. [EVIDENCE: Anthropic]

---

## 8. Local / open-model + continuous-batching specifics

This is where our setup diverges most from the frontier-API write-ups; the above sources assume metered APIs, we have **one model, fixed weights, shared GPU**.

- **All agents share one GPU batch.** Continuous (rolling) batching adds/removes sequences per decode step, maximizing utilization (benchmarks: **14–24× throughput over naive, 2–4× over Ollama at high concurrency**). This means our parallel sub-agents *are* the batch — fan-out concurrency directly trades against per-agent latency. Cap concurrency to the batch the model serves well. [EVIDENCE] (https://www.runpod.io/articles/guides/vllm-pagedattention-continuous-batching)
- **⚠ Token cost ≠ dollars; it's latency/throughput.** Anthropic's "~15× tokens" cost argument is a *budget* concern on APIs; for us the constraint is **GPU time and the concurrency knee**, not a bill. Re-frame all "is the parallelism worth the tokens?" decisions as "is it worth the batch slots / wall-clock?" [INFERENCE]
- **Exploit shared-prefix KV caching.** Agents that share a long, identical system/role prefix get prefix-cache reuse for free. **KVFlow** (NeurIPS 2025) makes this workflow-aware (Agent Step Graph + steps-to-execution eviction + overlapped CPU→GPU prefetch): **up to 1.83× single-workflow, 2.19× many-concurrent-workflow speedups over SGLang radix cache.** Design prompts so the *stable* content is a long shared prefix and the *variable* part is short and trailing; reuse identical role prompts across many sub-agents. [EVIDENCE] (https://arxiv.org/abs/2507.07400)
- **Role specialization without different models.** On frontier stacks you'd use Opus-lead/Sonnet-workers and different judge families. We can't swap weights, so specialize via **system prompts, few-shot rubrics, tool allowlists, and decoding params** (e.g., higher temperature for ideators, low/zero for verifiers). Perspective diversity for verification comes from *prompts/personas*, not model family. [INFERENCE; the role-split intent is EVIDENCE from Anthropic]
- **Prompt-format discipline is stricter on open models.** Smaller/local models are more brittle to format drift than frontier models; keep tool schemas tight, outputs schema-constrained (grammar/JSON-mode if available), and few-shot the exact handoff payload format. [INFERENCE]
- **When one strong agent beats many weak agents.** With a *single* local model, "multi-agent" doesn't add a *more capable* model — it only adds parallelism + context isolation, while *multiplying* the same model's error rate and coordination cost. So the bar to fan out is **higher** than on a heterogeneous frontier stack. Prefer one well-contexted agent unless the task is genuinely independent-parallel (researchers, independent verifiers). [INFERENCE, strongly implied by §1 + error-compounding in §7]
- **Smaller models can still run agents** (e.g., 1.7B-class served under continuous batching), but capability per call is lower — push *more structure and narrower tasks* onto each sub-agent. [EVIDENCE for feasibility; INFERENCE for the design consequence]

---

## 9. Frameworks landscape (mid-2026) — what each *encodes*

We're not necessarily adopting one; we want the patterns they bake in. (Sources: 2026 framework comparisons, see links.)

- **LangGraph** — directed graph + conditional edges; **built-in checkpointing/time-travel, streaming, LangSmith observability.** Encodes: explicit state machine, durable execution, supervisor/subagent patterns. Best for stateful production workflows; steeper learning curve. [EVIDENCE]
- **OpenAI Agents SDK** (successor to the Swarm experiment) — clean, opinionated; **explicit handoffs vs agents-as-tools distinction**, built-in tracing + guardrails; ephemeral context variables. Encodes: handoff-centric orchestration. OpenAI-model-centric. [EVIDENCE]
- **CrewAI** — role-based "crews" + process types; lowest learning curve (role DSL, ~20 lines). Encodes: role specialization + sequential/hierarchical process. Best for rapid business-process prototyping; ~30–60% faster than AutoGen on simple orchestration. [EVIDENCE]
- **AutoGen / AG2** — conversational GroupChat; in-memory conversation history. Encodes: multi-agent *conversation* as the coordination primitive; strong on complex multi-turn negotiation. [EVIDENCE]
- **Claude Agent SDK** — tool-use chains + sub-agents, MCP-based state, safety-first + extended thinking. Encodes: orchestrator-worker + filesystem-artifact + compaction patterns from Anthropic's own research system. [EVIDENCE]
- **Convergence (2026):** all six now support **MCP**, streaming, some persistence/observability, and the **ReAct** loop. The differentiators are state model (graph/checkpoint vs conversation vs ephemeral) and orchestration primitive (graph vs handoff vs crew vs groupchat). [EVIDENCE]

**Patterns worth stealing for our home-grown system:** LangGraph's **checkpointing + explicit state schema**; OpenAI SDK's **clean agent-as-tool vs handoff separation + guardrails/tracing**; Anthropic SDK's **filesystem-artifact + compaction**; CrewAI's **terse role contracts**. [INFERENCE]

---

## Concrete recommendation for our topology (synthesis)

1. **Orchestrator = single writer**, agent-as-tool style. Holds plan + distilled findings + the artifact. Embeds scaling rules (effort ∝ complexity) and hard caps. [EVIDENCE-backed pattern]
2. **Researchers = fan-out, read-only.** Each gets a typed contract (objective/output-schema/tool-allowlist/boundaries), returns a compact structured payload (claims+evidence+confidence+refs) to a blackboard. [EVIDENCE]
3. **Ideators = fan-out, candidate-only.** Higher temperature; each emits an independent candidate + stated assumptions. **No co-writing.** A selection/merge step (the orchestrator) picks/combines. [INFERENCE]
4. **Verifiers = fan-out, read-only, perspective-diverse.** Different rubrics/personas (model family is fixed); order-swap for any pairwise judgment; calibrated against a small labeled set; human spot-check on failures. [EVIDENCE]
5. **Verify-before-commit loop** with bounded iterations + completeness critic + "stop when no new info." [EVIDENCE/INFERENCE]
6. **Context discipline:** distilled handoffs, compaction, artifacts-to-disk + refs, per-role tool allowlists. [EVIDENCE]
7. **Local-serving discipline:** long shared role-prefixes for KV reuse; concurrency capped to the batch knee; schema-constrained outputs; decode params per role; OTel tracing + token/turn circuit breakers. [EVIDENCE/INFERENCE]

---

## Sources (URL + date)

Primary / engineering write-ups (load-bearing):
- Anthropic — *How we built our multi-agent research system* (June 2025): https://www.anthropic.com/engineering/built-multi-agent-research-system
- Cognition (Walden Yan) — *Don't Build Multi-Agents* (June 2025): https://cognition.com/blog/dont-build-multi-agents
- LangChain — *How and when to build multi-agent systems* (June 2025): https://www.langchain.com/blog/how-and-when-to-build-multi-agent-systems
- Anthropic — *Effective context engineering for AI agents* (Sept 2025): https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents
- Anthropic — *Code execution with MCP* (2025): https://www.anthropic.com/engineering/code-execution-with-mcp
- KVFlow — *Efficient Prefix Caching for LLM-Based Multi-Agent Workflows*, arXiv 2507.07400 (July 2025; NeurIPS 2025): https://arxiv.org/abs/2507.07400

Supporting (2025–2026):
- Morph — *Context Rot: Why LLMs Degrade as Context Grows* (2025): https://www.morphllm.com/context-rot
- EclipseSource — *MCP and Context Overload* (Jan 2026): https://eclipsesource.com/blogs/2026/01/22/mcp-context-overload/
- Future AGI — *LLM-as-a-Judge in 2026: How It Works, When It Fails* (2026): https://futureagi.com/blog/llm-as-a-judge/
- RunPod — *vLLM: PagedAttention and Continuous Batching* (2026): https://www.runpod.io/articles/guides/vllm-pagedattention-continuous-batching
- Beam.ai — *6 Multi-Agent Orchestration Patterns for Production* (2026): https://beam.ai/agentic-insights/multi-agent-orchestration-patterns-production
- Zylos Research — *Parallel Concurrency in Production AI Agents: DAG Scheduling, Fan-Out/Fan-In* (Apr 2026): https://zylos.ai/research/2026-04-26-parallel-concurrency-agent-execution/
- digitalapplied — *Agent Observability 2026: Evals, Traces, Cost* (2026): https://www.digitalapplied.com/blog/agent-observability-2026-evals-traces-cost-guide
- Future AGI — *Multi-Agent Tracing 2026: OTel, Span Hierarchy* (2026): https://futureagi.com/blog/trace-debug-multi-agent-systems-observability-guide/
- QubitTool — *2026 AI Agent Framework Showdown* (2026): https://qubittool.com/blog/ai-agent-framework-comparison-2026
- arXiv 2509.17995 — *Variation in Verification: Verification Dynamics in LLMs* (Sept 2025): https://arxiv.org/pdf/2509.17995
- arXiv 2512.22245 — *Calibrating LLM Judges: Linear Probes for Uncertainty* (2025): https://arxiv.org/pdf/2512.22245

*Note on dates:* publication months for the four anchor blog posts (Anthropic ×2, Cognition, LangChain) are well-established mid/late-2025; secondary 2026 trade sources are used for trend confirmation, not as sole support for any load-bearing claim.
