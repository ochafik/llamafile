#!/usr/bin/env python3
# Copyright 2026 Mozilla.ai - Apache-2.0
#
# Integration test for the interactive multi-agent runtime (Phase 1).
# Model-gated: needs a real model + ZIM. Starts the llamafile server with
# --server --jinja --agents --mcp '<self> mcp-server --zim ...', then drives a
# /runtime/start session whose orchestrator must fan out TWO researchers IN
# PARALLEL, await them, and synthesize. Proves from the trace:
#   (a) two spawn_agent calls + overlapping researcher turns (concurrency),
#   (b) researchers use wiki tools,
#   (c) results delivered back via message events,
#   (d) orchestrator awaits + emits a final answer,
#   (e) the agent tree (parent_ids) + summed token totals across all agents.
#
# usage: runtime_mesh_test.py <llamafile_bin> <model.gguf> <wiki.zim> [port]

import json, os, subprocess, sys, time, urllib.request, threading

def http(method, url, body=None, timeout=600):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read().decode()

def wait_health(base, secs=240):
    for _ in range(secs):
        try:
            s, _ = http("GET", base + "/health", timeout=5)
            if s == 200:
                return True
        except Exception:
            pass
        time.sleep(1)
    return False

def main():
    if len(sys.argv) < 4:
        print("usage: runtime_mesh_test.py <bin> <model> <zim> [port]", file=sys.stderr)
        return 2
    binp, model, zim = sys.argv[1], sys.argv[2], sys.argv[3]
    port = int(sys.argv[4]) if len(sys.argv) > 4 else 18181
    base = f"http://127.0.0.1:{port}"

    # APE binaries are run through /bin/sh (the polyglot prologue); posix_spawnp
    # of the raw path returns ENOEXEC without the execvp sh-fallback.
    mcp = f"/bin/sh '{binp}' mcp-server --zim '{zim}'"
    cmd = ["/bin/sh", binp, "--server", "--jinja", "--agents", "--mcp", mcp,
           "-m", model, "-np", "8", "--host", "127.0.0.1", "--port", str(port),
           "--nologo"]
    print("launching:", " ".join(cmd), file=sys.stderr)
    srv = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    events = []
    stop = threading.Event()

    def reader_log():
        for line in srv.stdout:
            if stop.is_set():
                break
            sys.stderr.write("[srv] " + line.decode(errors="replace"))

    log_t = threading.Thread(target=reader_log, daemon=True)
    log_t.start()

    rc = 1
    try:
        if not wait_health(base):
            print("server did not become healthy", file=sys.stderr)
            return 1

        # SSE consumer on /runtime/events
        def sse():
            try:
                with urllib.request.urlopen(base + "/runtime/events", timeout=900) as r:
                    for raw in r:
                        if stop.is_set():
                            break
                        s = raw.decode(errors="replace").strip()
                        if s.startswith("data: "):
                            try:
                                events.append(json.loads(s[6:]))
                            except Exception:
                                pass
            except Exception as e:
                sys.stderr.write(f"[sse] closed: {e}\n")

        goal = ("Spawn TWO researcher sub-agents to work IN PARALLEL: one to find the "
                "height of the Eiffel Tower, one to find the height of the Statue of "
                "Liberty. Use wiki_search/wiki_get_article. Await both, then compare "
                "their heights and state which is taller.")
        s, b = http("POST", base + "/runtime/start", {"goal": goal, "turn_budget": 60})
        print("start ->", s, b, file=sys.stderr)
        start = json.loads(b)
        orch = start.get("orchestrator")

        # SSE replays the full trace from the start of the session, so opening it
        # after /runtime/start still captures every event.
        sse_t = threading.Thread(target=sse, daemon=True)
        sse_t.start()

        # wait for a final answer (or timeout)
        deadline = time.time() + 480
        final = None
        while time.time() < deadline:
            try:
                s, b = http("GET", base + "/runtime/status", timeout=10)
                st = json.loads(b)
                if st.get("final_answer"):
                    final = st
                    break
            except Exception:
                pass
            time.sleep(2)

        time.sleep(1)
        stop.set()

        # pull the authoritative trace
        try:
            _, tb = http("GET", base + "/runtime/trace", timeout=10)
            trace = [json.loads(l) for l in tb.splitlines() if l.strip()]
        except Exception:
            trace = events

        rc = analyze(trace, orch, final)
    finally:
        stop.set()
        try:
            http("POST", base + "/runtime/stop", {})
        except Exception:
            pass
        srv.terminate()
        try:
            srv.wait(timeout=10)
        except Exception:
            srv.kill()
    return rc

def analyze(trace, orch, final):
    print("\n================= TRACE ANALYSIS =================")
    spawns = [e for e in trace if e.get("type") == "spawn"]
    turns  = [e for e in trace if e.get("type") == "turn"]
    tcalls = [e for e in trace if e.get("type") == "tool_call"]
    msgs   = [e for e in trace if e.get("type") == "message"]
    awaits = [e for e in trace if e.get("type") == "await"]

    # agent tree
    print("\n--- agent tree (parent_id) ---")
    names = {}
    for e in spawns:
        names[e["agent_id"]] = f'{e.get("name","?")}[{e.get("role","?")}]'
    for e in spawns:
        pid = e.get("parent_id") or "(root)"
        pn = names.get(pid, pid) if pid != "(root)" else "(root)"
        print(f'  {pn}  ->  {names[e["agent_id"]]}  id={e["agent_id"]} depth={e.get("depth")}')

    # researchers = children of the orchestrator
    researchers = [e["agent_id"] for e in spawns if e.get("parent_id") == orch]
    print(f"\northestrator={orch}  researchers={researchers}")

    # (a) two spawns in flight: spawn timestamps close together
    ok_two_spawn = len(researchers) >= 2
    if len(spawns) >= 2:
        dt = abs(spawns[0]["ts"] - spawns[1]["ts"])
        print(f"two spawn_agent calls dt={dt*1000:.1f} ms")

    # concurrency: do the two researchers' turn intervals overlap in time?
    def turns_for(aid):
        out = []
        for e in turns:
            if e.get("agent_id") == aid:
                end = e["ts"]
                start = end - e.get("dur_ms", 0) / 1000.0
                out.append((start, end))
        return out

    overlap = False
    if len(researchers) >= 2:
        A = turns_for(researchers[0])
        B = turns_for(researchers[1])
        for (a0, a1) in A:
            for (b0, b1) in B:
                if a0 < b1 and b0 < a1:
                    overlap = True
        print("\n--- researcher turn windows (epoch s) ---")
        for r in researchers[:2]:
            for (s0, s1) in turns_for(r):
                print(f"  {names.get(r,r)}: [{s0:.3f} .. {s1:.3f}]  ({(s1-s0)*1000:.0f} ms)")
        print(f"researcher turns OVERLAP in time: {overlap}")

    # (b) researchers use wiki tools
    wiki = [e for e in tcalls if e.get("agent_id") in researchers and
            str(e.get("tool","")).startswith("wiki")]
    print(f"\nwiki tool calls by researchers: {len(wiki)} -> "
          f"{sorted(set(e['tool'] for e in wiki))}")

    # (c) results delivered back to orchestrator
    back = [e for e in msgs if e.get("to") == orch and e.get("from") in researchers]
    print(f"messages delivered back to orchestrator from researchers: {len(back)}")

    # (d) await + final
    print(f"await events: {len(awaits)}")
    fa = final.get("final_answer") if final else ""
    print("\n--- FINAL ANSWER ---\n" + (fa[:800] if fa else "(none)"))

    # (e) token totals across all agents
    if final:
        print("\n--- SESSION TOKEN TOTALS (summed across ALL agents) ---")
        print(f'  n_agents={final.get("n_agents")} n_turns={final.get("n_turns")} '
              f'prompt={final.get("total_prompt_tokens")} '
              f'completion={final.get("total_completion_tokens")} '
              f'total={final.get("total_tokens")}')
        # cross-check: sum trace turn token fields
        sp = sum(e.get("tokens_prompt", 0) for e in turns)
        sc = sum(e.get("tokens_completion", 0) for e in turns)
        print(f'  trace.jsonl turn-sum: prompt={sp} completion={sc} total={sp+sc}')

    checks = {
        "two researchers spawned": ok_two_spawn,
        "researcher turns overlap (concurrency)": overlap,
        "researchers used wiki tools": len(wiki) >= 1,
        "results delivered back to orchestrator": len(back) >= 1,
        "orchestrator awaited": len(awaits) >= 1,
        "final answer produced": bool(fa),
        "token totals present": bool(final and final.get("total_tokens", 0) > 0),
    }
    print("\n================= CHECKS =================")
    allok = True
    for k, v in checks.items():
        print(f'  [{"PASS" if v else "FAIL"}] {k}')
        allok = allok and v
    return 0 if allok else 1

if __name__ == "__main__":
    sys.exit(main())
