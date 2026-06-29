#!/usr/bin/env python3
# Copyright 2026 Mozilla.ai - Apache-2.0
#
# Agentic-flow EVAL harness (design: ddocs/09-interactive-multiagent-runtime.md
# §11). Beyond the hermetic unit/integration tests, this is a *behavioral* eval:
# it runs simple, deterministically-gradable objectives many times against a live
# llamafile runtime and measures reliability + the multi-agent token multiplier
# (the ddoc 03 concern). Model-gated; NOT in `make check`.
#
# How it drives the runtime (Phase-1 Drive API, see agent_runtime_server.cpp):
#   - POST /runtime/start {goal, turn_budget} -> creates a fresh session + an
#     orchestrator agent (tools: spawn_agent/send_message/await/list_agents +
#     wiki_*/wikidata_* via --mcp). Returns {session, orchestrator}.
#   - GET  /runtime/status -> {n_agents, n_turns, total_prompt_tokens,
#     total_completion_tokens, total_tokens, agents[], final_answer}. The token
#     totals are accumulated by Runtime::add_usage for EVERY agent (orchestrator
#     + every spawned sub-agent), so total_tokens is summed across ALL agents,
#     not just the orchestrator.
#   - POST /runtime/stop -> tears the session down.
# We poll /runtime/status until final_answer appears (or timeout), grade the
# final answer with a per-objective substring/regex grader, and record
# total_tokens / n_agents / n_turns / wall-clock. Fresh session per attempt.
#
# usage (typically via tests/eval/run.sh, which boots the server):
#   agentic_flows.py --base http://127.0.0.1:18182 \
#       [--attempts N] [--objectives E1,E3] [--timeout 180] [--turn-budget 60]
# or let it boot the server itself:
#   agentic_flows.py --bin <llamafile> --model <m.gguf> --zim <z.zim> \
#       --wikidata <wd.sqlite> [--port 18182] [--attempts N] [--objectives ...]

import argparse, json, os, re, signal, statistics, subprocess, sys, threading, time, urllib.request


# --------------------------------------------------------------------------- #
# HTTP helpers
# --------------------------------------------------------------------------- #
def http(method, url, body=None, timeout=60):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read().decode()


def wait_health(base, secs=300):
    for _ in range(secs):
        try:
            s, _ = http("GET", base + "/health", timeout=5)
            if s == 200:
                return True
        except Exception:
            pass
        time.sleep(1)
    return False


# --------------------------------------------------------------------------- #
# Graders: each takes the final-answer text, returns True iff it passes.
# Deterministic substring/regex matching (no LLM judge).
# --------------------------------------------------------------------------- #
def grade_e1(text):
    # Height of the Eiffel Tower: ~330 m (with antenna), 324 m, or ~300 m base.
    return bool(re.search(r"\b(330|324|312|300)\s*m", text, re.I))


def grade_e2(text):
    return "france" in text.lower()


def grade_e3(text):
    # Must mention Eiffel and indicate Eiffel is the taller of the two.
    t = text.lower()
    if "eiffel" not in t:
        return False
    if not re.search(r"tall|high", t):
        return False
    # Eiffel associated with "taller/tallest" (either order, within a clause), or
    # an explicit "eiffel is taller" / "eiffel tower is taller" statement.
    if re.search(r"eiffel[^.!?\n]{0,60}\b(tall|high)", t):
        return True
    if re.search(r"\b(tall|high)[^.!?\n]{0,60}eiffel", t):
        return True
    return False


def grade_e4(text):
    # Claim is TRUE (Eiffel ~330 m > Statue of Liberty ~93 m). Pass iff the
    # verdict is TRUE (not FALSE) and at least one height (a number + m) is given.
    t = text.lower()
    says_true = bool(re.search(r"\btrue\b", t))
    says_false = bool(re.search(r"\bfalse\b", t))
    has_height = bool(re.search(r"\d{2,3}\s*m\b", t))
    # If both TRUE and FALSE appear (e.g. "TRUE or FALSE" echoed), require TRUE to
    # be the clear verdict: appear after the last FALSE, or appear more often.
    verdict_true = says_true and not (says_false and t.rfind("false") > t.rfind("true"))
    return verdict_true and has_height


OBJECTIVES = {
    "E1": dict(
        goal="What is the height of the Eiffel Tower? Use the wiki tools.",
        grader=grade_e1, kind="single-agent (wiki)"),
    "E2": dict(
        goal="What country is the Eiffel Tower in? Use Wikidata.",
        grader=grade_e2, kind="single-agent (wikidata)"),
    "E3": dict(
        goal=("Compare the heights of the Eiffel Tower and the Statue of Liberty; "
              "which is taller? Spawn TWO researcher sub-agents to work in parallel "
              "(one per landmark) using the wiki tools, await both, then state which "
              "is taller."),
        grader=grade_e3, kind="multi-agent (2 researchers)"),
    "E4": dict(
        goal=("Verify this claim: the Eiffel Tower is taller than the Statue of "
              "Liberty. Spawn a researcher to find both heights and a verifier to "
              "check the claim. Answer TRUE or FALSE and give the two heights."),
        grader=grade_e4, kind="multi-agent (researcher+verifier)"),
}


# --------------------------------------------------------------------------- #
# One attempt: start a fresh session, poll to a final answer, grade it.
# --------------------------------------------------------------------------- #
def run_attempt(base, goal, grader, timeout, turn_budget):
    rec = dict(passed=False, total_tokens=0, prompt_tokens=0, completion_tokens=0,
               n_agents=0, n_turns=0, wall=0.0, final="", timed_out=False, error=None)
    t0 = time.time()
    try:
        s, b = http("POST", base + "/runtime/start",
                    {"goal": goal, "turn_budget": turn_budget}, timeout=30)
        if s != 200:
            rec["error"] = f"start http {s}: {b[:200]}"
            return rec
    except Exception as e:
        rec["error"] = f"start failed: {e}"
        return rec

    deadline = t0 + timeout
    st = None
    while time.time() < deadline:
        try:
            s, b = http("GET", base + "/runtime/status", timeout=15)
            st = json.loads(b)
            if st.get("final_answer"):
                break
        except Exception:
            pass
        time.sleep(2)

    rec["wall"] = time.time() - t0
    if st is not None:
        rec["total_tokens"] = int(st.get("total_tokens", 0) or 0)
        rec["prompt_tokens"] = int(st.get("total_prompt_tokens", 0) or 0)
        rec["completion_tokens"] = int(st.get("total_completion_tokens", 0) or 0)
        rec["n_agents"] = int(st.get("n_agents", 0) or 0)
        rec["n_turns"] = int(st.get("n_turns", 0) or 0)
        fa = st.get("final_answer") or ""
        rec["final"] = fa
        if fa:
            rec["passed"] = bool(grader(fa))
        else:
            rec["timed_out"] = True

    try:
        http("POST", base + "/runtime/stop", {}, timeout=15)
    except Exception:
        pass
    return rec


# --------------------------------------------------------------------------- #
# Reporting
# --------------------------------------------------------------------------- #
def _stats(vals):
    if not vals:
        return (0, 0, 0, 0)
    return (statistics.mean(vals), statistics.median(vals), min(vals), max(vals))


def report(results, attempts):
    """results: {obj_id: [rec, ...]}. Prints per-objective table + multiplier."""
    print("\n" + "=" * 96)
    print("AGENTIC-FLOW EVAL REPORT  (tokens summed across ALL agents per run)")
    print("=" * 96)
    hdr = (f"{'obj':<4} {'flow':<28} {'pass':>7} {'tok_mean':>9} {'tok_med':>8} "
           f"{'tok_min':>8} {'tok_max':>8} {'agents':>6} {'turns':>6} {'wall_s':>7}")
    print(hdr)
    print("-" * len(hdr))
    means = {}
    for oid in results:
        recs = results[oid]
        n = len(recs)
        npass = sum(1 for r in recs if r["passed"])
        toks = [r["total_tokens"] for r in recs if r["total_tokens"] > 0]
        tm, tmed, tmin, tmax = _stats(toks)
        means[oid] = tm
        ag = statistics.mean([r["n_agents"] for r in recs]) if recs else 0
        tn = statistics.mean([r["n_turns"] for r in recs]) if recs else 0
        wl = statistics.mean([r["wall"] for r in recs]) if recs else 0
        kind = OBJECTIVES[oid]["kind"]
        print(f"{oid:<4} {kind:<28} {npass:>3}/{n:<3} {tm:>9.0f} {tmed:>8.0f} "
              f"{tmin:>8.0f} {tmax:>8.0f} {ag:>6.1f} {tn:>6.1f} {wl:>7.1f}")
    print("-" * len(hdr))

    # multi-agent multiplier = E3 mean tokens / E1 mean tokens (single baseline).
    if means.get("E1") and means.get("E3"):
        mult = means["E3"] / means["E1"]
        print(f"\nMULTI-AGENT MULTIPLIER  E3/E1 = {means['E3']:.0f}/{means['E1']:.0f} "
              f"= {mult:.2f}x  (multi-agent token cost vs single-agent baseline)")
    else:
        print("\nMULTI-AGENT MULTIPLIER  E3/E1 = n/a (need both E1 and E3 in the run)")

    # errors / timeouts summary
    print("\nnotes:")
    for oid in results:
        errs = [r for r in results[oid] if r["error"]]
        tos = [r for r in results[oid] if r["timed_out"]]
        if errs:
            print(f"  {oid}: {len(errs)} attempt(s) errored, first: {errs[0]['error']}")
        if tos:
            print(f"  {oid}: {len(tos)} attempt(s) timed out (no final answer)")
    print("=" * 96)


# --------------------------------------------------------------------------- #
# Optional server boot (mirrors tests/integration/runtime_mesh_test.py)
# --------------------------------------------------------------------------- #
def boot_server(bin_, model, zim, wikidata, port, np_, ctx):
    # The combined mcp-server exposes wiki_* (from --zim) and wikidata_* (from
    # --wikidata) in one subprocess. APE binaries launch through /bin/sh.
    mcp_parts = [f"/bin/sh '{bin_}' mcp-server"]
    if zim:
        mcp_parts.append(f"--zim '{zim}'")
    if wikidata:
        mcp_parts.append(f"--wikidata '{wikidata}'")
    mcp = " ".join(mcp_parts)
    cmd = ["/bin/sh", bin_, "--server", "--jinja", "--agents", "--mcp", mcp,
           "-m", model, "-np", str(np_), "-c", str(ctx),
           "--host", "127.0.0.1", "--port", str(port), "--nologo"]
    print("launching:", " ".join(cmd), file=sys.stderr)
    srv = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    stop = threading.Event()

    def pump():
        for line in srv.stdout:
            if stop.is_set():
                break
            sys.stderr.write("[srv] " + line.decode(errors="replace"))

    threading.Thread(target=pump, daemon=True).start()
    return srv, stop


def main():
    ap = argparse.ArgumentParser(description="agentic-flow EVAL harness")
    ap.add_argument("--base", help="base URL of an already-running runtime "
                    "(skip server boot), e.g. http://127.0.0.1:18182")
    ap.add_argument("--bin", help="llamafile binary (to boot a server)")
    ap.add_argument("--model", help="model .gguf (to boot a server)")
    ap.add_argument("--zim", help="wikipedia .zim (to boot a server)")
    ap.add_argument("--wikidata", help="wikidata .sqlite (to boot a server)")
    ap.add_argument("--port", type=int, default=18182)
    ap.add_argument("--np", type=int, default=8, help="-np slots")
    ap.add_argument("--ctx", type=int, default=8192, help="-c context")
    ap.add_argument("--attempts", type=int, default=10, help="attempts per objective")
    ap.add_argument("--objectives", default="E1,E2,E3,E4",
                    help="comma list of objective ids to run")
    ap.add_argument("--timeout", type=int, default=180, help="per-attempt seconds")
    ap.add_argument("--turn-budget", type=int, default=60, help="runtime turn budget")
    args = ap.parse_args()

    obj_ids = [o.strip() for o in args.objectives.split(",") if o.strip()]
    for o in obj_ids:
        if o not in OBJECTIVES:
            print(f"unknown objective {o}; known: {list(OBJECTIVES)}", file=sys.stderr)
            return 2

    srv = stop = None
    base = args.base
    try:
        if not base:
            if not (args.bin and args.model):
                print("need --base OR (--bin and --model [...])", file=sys.stderr)
                return 2
            srv, stop = boot_server(args.bin, args.model, args.zim, args.wikidata,
                                    args.port, args.np, args.ctx)
            base = f"http://127.0.0.1:{args.port}"
        if not wait_health(base):
            print("server did not become healthy", file=sys.stderr)
            return 1

        print(f"\nrunning {len(obj_ids)} objective(s) x {args.attempts} attempt(s) "
              f"against {base}", file=sys.stderr)
        results = {}
        for oid in obj_ids:
            spec = OBJECTIVES[oid]
            results[oid] = []
            for k in range(args.attempts):
                rec = run_attempt(base, spec["goal"], spec["grader"],
                                  args.timeout, args.turn_budget)
                tag = "PASS" if rec["passed"] else ("TIMEOUT" if rec["timed_out"]
                                                    else ("ERR" if rec["error"] else "FAIL"))
                print(f"  {oid} attempt {k+1}/{args.attempts}: {tag}  "
                      f"tokens={rec['total_tokens']} agents={rec['n_agents']} "
                      f"turns={rec['n_turns']} wall={rec['wall']:.0f}s",
                      file=sys.stderr)
                if rec["final"]:
                    snippet = rec["final"][:200].replace("\n", " ")
                    print(f"        final: {snippet}", file=sys.stderr)
                results[oid].append(rec)

        report(results, args.attempts)
        # exit 0 always (eval is a measurement, not a gate); regressions are read
        # from the table. A nonzero exit only on infra failure (handled above).
        return 0
    finally:
        if stop:
            stop.set()
        if srv:
            try:
                http("POST", (base or "") + "/runtime/stop", {}, timeout=5)
            except Exception:
                pass
            srv.terminate()
            try:
                srv.wait(timeout=10)
            except Exception:
                srv.kill()


if __name__ == "__main__":
    sys.exit(main())
