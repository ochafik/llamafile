#!/usr/bin/env python3
"""
Multi-agent orchestration harness for a SINGLE llamafile server (continuous batching).

Implements the "agent-as-tool" pattern against an OpenAI-compatible /v1/chat/completions
endpoint:

  orchestrator  (talks to user; tools: delegate_to_ideator / _researcher / _verifier + answer_user)
    └─ ideator   (brainstorm; tools: none)
    └─ researcher(tools: wiki_search, browser_open, web_fetch)
    └─ verifier  (adversarial re-check; tools: wiki_search, web_fetch)

Each sub-agent is exposed to the orchestrator AS A TOOL. Calling that tool runs the
sub-agent's own inner tool loop on the SAME model server and returns its final text.
Sub-agent tool calls in one orchestrator turn are dispatched CONCURRENTLY (asyncio),
which is exactly what exercises the server's continuous batching: N in-flight
/v1/chat/completions requests share the batched decode.

Two modes:
  --mock   : deterministic fake LLM (canned tool_calls) -> exercises the FULL control
             flow with no model. Use this to see the orchestration logic right now.
  (default): real mode -> point --base-url at the llamafile server.

Usage:
  python orchestrate.py --mock
  python orchestrate.py --base-url http://localhost:8080/v1 --model qwen3.6-35b-a3b \
      --task "Compare the energy mix of France and Germany in 2024."
"""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time
from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable

# ----------------------------------------------------------------------------
# Tiny event log (stands in for the UI's per-agent lanes / streamed status)
# ----------------------------------------------------------------------------
_T0 = time.time()


def log(agent: str, kind: str, msg: str) -> None:
    dt = time.time() - _T0
    indent = {"orchestrator": "", "ideator": "    ", "researcher": "    ", "verifier": "    "}.get(
        agent.split("#")[0], "    "
    )
    print(f"[{dt:6.2f}s] {indent}{agent:<14} {kind:<10} {msg}", flush=True)


# ----------------------------------------------------------------------------
# Leaf tools (stubs). In llamafile these are server_tool registry entries
# (wiki_search inlined; browser_* from the browser-use agent; web_fetch builtin).
# ----------------------------------------------------------------------------
async def tool_wiki_search(query: str = "", limit: int = 3, **_: Any) -> str:
    await asyncio.sleep(0.05)  # simulate I/O
    return json.dumps(
        [{"title": f"{query} (overview)", "snippet": f"Stub wiki snippet about '{query}'."}][:limit]
    )


async def tool_browser_open(url: str = "", **_: Any) -> str:
    await asyncio.sleep(0.05)
    return f"[browser_open stub] Loaded {url!r}; page text: 'Lorem ipsum about {url}'."


async def tool_web_fetch(url: str = "", **_: Any) -> str:
    await asyncio.sleep(0.05)
    return f"[web_fetch stub] {url!r} -> 'Fetched content summary for {url}'."


LEAF_TOOLS: dict[str, Callable[..., Awaitable[str]]] = {
    "wiki_search": tool_wiki_search,
    "browser_open": tool_browser_open,
    "web_fetch": tool_web_fetch,
}

# OpenAI tool schemas for the leaf tools (subset, per-role allowlisted below).
LEAF_SCHEMAS: dict[str, dict] = {
    "wiki_search": {
        "type": "function",
        "function": {
            "name": "wiki_search",
            "description": "Search the offline Wikipedia ZIM. Returns [{title, snippet}].",
            "parameters": {
                "type": "object",
                "properties": {
                    "query": {"type": "string"},
                    "limit": {"type": "integer", "default": 3},
                },
                "required": ["query"],
            },
        },
    },
    "browser_open": {
        "type": "function",
        "function": {
            "name": "browser_open",
            "description": "Open a URL in a headless browser and return visible page text.",
            "parameters": {
                "type": "object",
                "properties": {"url": {"type": "string"}},
                "required": ["url"],
            },
        },
    },
    "web_fetch": {
        "type": "function",
        "function": {
            "name": "web_fetch",
            "description": "Fetch a URL and return a text summary.",
            "parameters": {
                "type": "object",
                "properties": {"url": {"type": "string"}},
                "required": ["url"],
            },
        },
    },
}

# ----------------------------------------------------------------------------
# Roles: system prompt + tool allowlist
# ----------------------------------------------------------------------------
ROLE_PROMPTS = {
    "orchestrator": (
        "You are the ORCHESTRATOR. You talk to the user. Decompose the task, delegate to "
        "sub-agents via the delegate_* tools (you may call several in ONE turn to run them "
        "in parallel), reconcile their results, then call answer_user with the final answer. "
        "Never do research yourself; delegate it."
    ),
    "ideator": (
        "You are an IDEATOR. Brainstorm angles, hypotheses and sub-questions. You have NO "
        "external tools. Return a concise bulleted list."
    ),
    "researcher": (
        "You are a RESEARCHER. Use wiki_search, browser_open and web_fetch to gather facts. "
        "Cite sources. Return a short factual summary."
    ),
    "verifier": (
        "You are a VERIFIER. Adversarially re-check the given claims against wiki_search / "
        "web_fetch. Flag anything unsupported. Return PASS/FAIL per claim with evidence."
    ),
}

ROLE_TOOLS = {
    "orchestrator": [],  # gets the delegate_* + answer_user tools, injected dynamically
    "ideator": [],
    "researcher": ["wiki_search", "browser_open", "web_fetch"],
    "verifier": ["wiki_search", "web_fetch"],
}


# ----------------------------------------------------------------------------
# LLM client: real (OpenAI-compatible) or mock
# ----------------------------------------------------------------------------
@dataclass
class LLMResponse:
    content: str | None
    tool_calls: list[dict] = field(default_factory=list)


class RealLLM:
    """Non-streaming OpenAI-compatible client (httpx). Streaming maps 1:1 in the real UI."""

    def __init__(self, base_url: str, model: str, api_key: str = "no-key"):
        import httpx  # lazy import so --mock needs no deps

        self.base_url = base_url.rstrip("/")
        self.model = model
        self._client = httpx.AsyncClient(timeout=600.0, headers={"Authorization": f"Bearer {api_key}"})

    async def chat(self, messages: list[dict], tools: list[dict] | None) -> LLMResponse:
        body: dict[str, Any] = {"model": self.model, "messages": messages, "temperature": 0.6}
        if tools:
            body["tools"] = tools
            body["tool_choice"] = "auto"
        r = await self._client.post(f"{self.base_url}/chat/completions", json=body)
        r.raise_for_status()
        msg = r.json()["choices"][0]["message"]
        return LLMResponse(content=msg.get("content"), tool_calls=msg.get("tool_calls") or [])

    async def aclose(self) -> None:
        await self._client.aclose()


class MockLLM:
    """
    Deterministic fake model. Decides what to "say" from the system role + conversation
    so the FULL agent-as-tool control flow runs with no server. Latency simulates decode
    so concurrent sub-agents visibly overlap (continuous batching analogue).
    """

    def __init__(self) -> None:
        self._researcher_calls = 0

    async def chat(self, messages: list[dict], tools: list[dict] | None) -> LLMResponse:
        await asyncio.sleep(0.15)  # simulate generation latency
        system = messages[0]["content"] if messages and messages[0]["role"] == "system" else ""
        role = next((r for r in ROLE_PROMPTS if ROLE_PROMPTS[r][:20] in system), "orchestrator")
        last_tool = next((m for m in reversed(messages) if m["role"] == "tool"), None)

        if role == "orchestrator":
            # Turn 1: fan out to ideator + two researchers concurrently.
            if last_tool is None:
                return LLMResponse(
                    content="Planning: brainstorm, then research two facets in parallel.",
                    tool_calls=[
                        _tc("c1", "delegate_to_ideator", {"task": "angles on the user question"}),
                        _tc("c2", "delegate_to_researcher", {"task": "facet A: France energy mix 2024"}),
                        _tc("c3", "delegate_to_researcher", {"task": "facet B: Germany energy mix 2024"}),
                    ],
                )
            # Turn 2: results are in -> verify the synthesized claims.
            done = sum(1 for m in messages if m["role"] == "tool")
            if not any("delegate_to_verifier" in (m.get("content") or "") for m in messages) and done >= 3 and done < 4:
                return LLMResponse(
                    content="Synthesizing, then verifying key claims.",
                    tool_calls=[_tc("c4", "delegate_to_verifier",
                                    {"claims": "France ~nuclear-heavy; Germany ~renewables+gas"})],
                )
            # Turn 3: final answer to user.
            return LLMResponse(
                content=None,
                tool_calls=[_tc("c9", "answer_user", {
                    "answer": "France's 2024 mix is nuclear-dominated; Germany leans renewables with "
                              "gas backup. (synthesized from 2 researchers + verifier)"})],
            )

        if role == "ideator":
            return LLMResponse(content="- angle 1\n- angle 2\n- counterpoint to check")

        if role == "researcher":
            self._researcher_calls += 1
            if last_tool is None:
                # First turn: call two leaf tools.
                return LLMResponse(
                    content="Gathering sources.",
                    tool_calls=[
                        _tc("r1", "wiki_search", {"query": "energy mix 2024", "limit": 2}),
                        _tc("r2", "browser_open", {"url": "https://example.org/energy-2024"}),
                    ],
                )
            return LLMResponse(content="Summary: key facts gathered with 2 sources cited.")

        if role == "verifier":
            if last_tool is None:
                return LLMResponse(content="Checking.", tool_calls=[
                    _tc("v1", "wiki_search", {"query": "verify energy claim"})])
            return LLMResponse(content="PASS: both claims supported by wiki snippet.")

        return LLMResponse(content="(noop)")

    async def aclose(self) -> None:
        pass


def _tc(cid: str, name: str, args: dict) -> dict:
    return {"id": cid, "type": "function",
            "function": {"name": name, "arguments": json.dumps(args)}}


# ----------------------------------------------------------------------------
# The agentic loop (mirrors agenticStore.executeAgenticLoop), reused by ALL roles.
# ----------------------------------------------------------------------------
class Agent:
    def __init__(self, llm, role: str, name: str, registry: "Registry", max_turns: int = 6):
        self.llm = llm
        self.role = role
        self.name = name
        self.registry = registry
        self.max_turns = max_turns

    def _tools_for_role(self) -> list[dict]:
        tools = [LEAF_SCHEMAS[t] for t in ROLE_TOOLS[self.role]]
        if self.role == "orchestrator":
            tools += self.registry.delegate_schemas() + [ANSWER_USER_SCHEMA]
        return tools

    async def run(self, task: str) -> str:
        messages = [
            {"role": "system", "content": ROLE_PROMPTS[self.role]},
            {"role": "user", "content": task},
        ]
        tools = self._tools_for_role()
        log(self.name, "start", task[:70])
        for turn in range(self.max_turns):
            resp = await self.llm.chat(messages, tools or None)
            if not resp.tool_calls:
                log(self.name, "final", (resp.content or "")[:80])
                return resp.content or ""
            messages.append({"role": "assistant", "content": resp.content, "tool_calls": resp.tool_calls})
            names = ", ".join(c["function"]["name"] for c in resp.tool_calls)
            log(self.name, f"turn{turn}", f"calls: {names}")

            # answer_user terminates the orchestrator.
            au = next((c for c in resp.tool_calls if c["function"]["name"] == "answer_user"), None)
            if au:
                ans = json.loads(au["function"]["arguments"]).get("answer", "")
                log(self.name, "ANSWER", ans[:90])
                return ans

            # Dispatch ALL tool calls in this turn CONCURRENTLY (continuous batching).
            results = await asyncio.gather(*[self._dispatch(c) for c in resp.tool_calls])
            for call, result in zip(resp.tool_calls, results):
                messages.append({"role": "tool", "tool_call_id": call["id"], "content": result})
        log(self.name, "maxturns", "hit turn cap")
        return "(stopped: max turns)"

    async def _dispatch(self, call: dict) -> str:
        name = call["function"]["name"]
        args = json.loads(call["function"]["arguments"] or "{}")
        if name in LEAF_TOOLS:
            log(self.name, "tool->", name)
            out = await LEAF_TOOLS[name](**args)
            return out
        if name.startswith("delegate_to_"):
            sub_role = name[len("delegate_to_"):]
            return await self.registry.spawn(sub_role, args)
        return f"Error: unknown tool {name}"


ANSWER_USER_SCHEMA = {
    "type": "function",
    "function": {
        "name": "answer_user",
        "description": "Deliver the final answer to the user and end the orchestration.",
        "parameters": {"type": "object",
                       "properties": {"answer": {"type": "string"}},
                       "required": ["answer"]},
    },
}


class Registry:
    """Holds the model client + concurrency cap; turns sub-agents into orchestrator tools."""

    def __init__(self, llm, max_concurrency: int = 8):
        self.llm = llm
        self.sem = asyncio.Semaphore(max_concurrency)  # mirrors server -np slot budget
        self._counter = 0

    def delegate_schemas(self) -> list[dict]:
        def schema(role: str, arg: str, desc: str) -> dict:
            return {"type": "function", "function": {
                "name": f"delegate_to_{role}",
                "description": desc,
                "parameters": {"type": "object",
                               "properties": {arg: {"type": "string"}},
                               "required": [arg]}}}
        return [
            schema("ideator", "task", "Run an ideator sub-agent (brainstorm; no external tools)."),
            schema("researcher", "task", "Run a researcher sub-agent (wiki+browser+fetch)."),
            schema("verifier", "claims", "Run a verifier sub-agent (adversarial fact-check)."),
        ]

    async def spawn(self, role: str, args: dict) -> str:
        if role not in ROLE_PROMPTS:
            return f"Error: unknown sub-agent role {role}"
        self._counter += 1
        name = f"{role}#{self._counter}"
        task = args.get("task") or args.get("claims") or json.dumps(args)
        async with self.sem:  # block if all slots busy (no deadlock: caller is NOT holding a slot)
            log(name, "spawn", f"(slot acquired) {task[:50]}")
            agent = Agent(self.llm, role, name, self)
            result = await agent.run(task)
        log(name, "return", result[:70])
        return result


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mock", action="store_true", help="run with the deterministic fake LLM")
    ap.add_argument("--base-url", default="http://localhost:8080/v1")
    ap.add_argument("--model", default="qwen3.6-35b-a3b")
    ap.add_argument("--max-concurrency", type=int, default=8, help="mirror server -np slots")
    ap.add_argument("--task", default="Compare the energy mix of France and Germany in 2024.")
    args = ap.parse_args()

    llm = MockLLM() if args.mock else RealLLM(args.base_url, args.model)
    registry = Registry(llm, max_concurrency=args.max_concurrency)
    orchestrator = Agent(llm, "orchestrator", "orchestrator", registry, max_turns=8)

    print(f"=== Multi-agent orchestration ({'MOCK' if args.mock else 'REAL'}), "
          f"max_concurrency={args.max_concurrency} ===")
    print(f"User task: {args.task}\n")
    answer = await orchestrator.run(args.task)
    print("\n=== FINAL ANSWER TO USER ===")
    print(answer)
    await llm.aclose()
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
