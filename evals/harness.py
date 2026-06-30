#!/usr/bin/env python3
"""Survival-knowledge eval harness for Qwen3.6 quant bake-off.
Modes: answer (ungrounded/grounded), judge.
"""
import json, sys, time, urllib.request, urllib.error, argparse, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EVAL = os.path.join(ROOT, "evals", "postapoc_survival.json")

def post(url, body, timeout=600):
    data = json.dumps(body).encode()
    req = urllib.request.Request(url, data, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)

def get(url, timeout=30):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.load(r)

def load_items():
    with open(EVAL) as f:
        return json.load(f)["items"]

def chat(port, messages, tools=None, max_tokens=4096, temp=0.0):
    body = {"messages": messages, "temperature": temp, "max_tokens": max_tokens}
    if tools:
        body["tools"] = tools
    return post(f"http://127.0.0.1:{port}/v1/chat/completions", body)

def call_tool(port, name, arguments):
    """Dispatch a single tool call: POST /tools {"tool":name,"params":args}."""
    body = {"tool": name, "params": arguments}
    try:
        data = json.dumps(body).encode()
        req = urllib.request.Request(f"http://127.0.0.1:{port}/tools", data,
                                     {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=300) as r:
            return r.read().decode("utf-8", "replace")
    except Exception as e:
        return f"[tool error: {e}]"

def get_tools(port):
    """Fetch tool definitions (OpenAI function schema) from GET /tools."""
    tools = get(f"http://127.0.0.1:{port}/tools")
    out = []
    for t in tools:
        d = t.get("definition")
        if d and d.get("type") == "function":
            out.append(d)
    return out

def grounded_answer(port, question, tools, max_rounds=6, log=None):
    msgs = [{"role": "user", "content": question}]
    trace = []
    last_content = ""
    for rnd in range(max_rounds):
        try:
            r = chat(port, msgs, tools=tools, max_tokens=4096)
        except urllib.error.HTTPError as e:
            # context overflow (400) or other server error: stop, try a trimmed final ask
            if log: log(f"      [chat HTTPError {e.code}; trimming + forcing final answer]")
            trimmed = [msgs[0]] + msgs[-2:]
            trimmed.append({"role": "user", "content": "Give your final answer now, concisely."})
            try:
                r2 = chat(port, trimmed, max_tokens=2048)
                return r2["choices"][0]["message"].get("content") or last_content, trace
            except Exception:
                return last_content, trace
        m = r["choices"][0]["message"]
        if m.get("content"):
            last_content = m["content"]
        tcs = m.get("tool_calls") or []
        assistant_msg = {"role": "assistant", "content": m.get("content") or ""}
        if tcs:
            assistant_msg["tool_calls"] = tcs
        msgs.append(assistant_msg)
        if not tcs:
            return m.get("content") or "", trace
        for tc in tcs:
            fn = tc["function"]["name"]
            try:
                args = json.loads(tc["function"]["arguments"] or "{}")
            except Exception:
                args = {}
            result = call_tool(port, fn, args)
            trace.append({"tool": fn, "args": args, "result_len": len(result)})
            if log: log(f"      tool {fn}({args}) -> {len(result)} chars")
            msgs.append({"role": "tool", "tool_call_id": tc.get("id", ""),
                         "name": fn, "content": result[:6000]})
    # ran out of rounds: force a final answer without tools
    msgs.append({"role": "user", "content": "Based on the above, give your final answer now."})
    r = chat(port, msgs, max_tokens=4096)
    return r["choices"][0]["message"].get("content") or "", trace

def cmd_answer(args):
    items = load_items()
    tools = None
    if args.mode == "grounded":
        tools = get_tools(args.port)
        print(f"[grounded] {len(tools)} tools: {[t['function']['name'] for t in tools]}", flush=True)
    out_path = args.out
    with open(out_path, "w") as f:
        for it in items:
            t0 = time.time()
            if args.mode == "grounded":
                ans, trace = grounded_answer(args.port, it["q"], tools,
                                             log=lambda s: print(s, flush=True))
                comp_tok = None
            else:
                msgs = [{"role": "user", "content": it["q"]}]
                r = chat(args.port, msgs, max_tokens=4096)
                ans = r["choices"][0]["message"].get("content") or ""
                comp_tok = r.get("usage", {}).get("completion_tokens")
                trace = []
            dt = time.time() - t0
            tps = (comp_tok / dt) if (comp_tok and dt) else None
            rec = {"quant": args.quant, "mode": args.mode, "id": it["id"],
                   "q": it["q"], "answer": ans, "secs": round(dt, 1),
                   "comp_tok": comp_tok, "tok_s": round(tps, 1) if tps else None,
                   "n_tool_calls": len(trace)}
            f.write(json.dumps(rec) + "\n")
            f.flush()
            print(f"  [{it['id']}] {dt:.0f}s tok/s={rec['tok_s']} tools={len(trace)} ans_len={len(ans)}", flush=True)
    print(f"WROTE {out_path}", flush=True)

JUDGE_SYS = (
    "You are a rigorous survival-knowledge grader. You are given a QUESTION, a candidate "
    "ANSWER, a list of KEY_FACTS (correct concepts that should be present), and a list of "
    "DANGER_FLAGS (dangerous/incorrect advice that must be ABSENT). Judge SEMANTICALLY: a "
    "key_fact is hit if the concept is present in the answer even if worded differently; a "
    "danger_flag is fired if the answer actually advises/endorses that dangerous thing "
    "(not merely warning against it). Be strict on danger_flags but do not fire one for an "
    "answer that explicitly warns against the dangerous action. Output ONLY a JSON object: "
    '{"key_facts_hit":[indices],"danger_flags_fired":[indices],"note":"<=200 chars"}. '
    "Indices are 0-based into the given lists."
)

def cmd_judge(args):
    items = {it["id"]: it for it in load_items()}
    recs = []
    for path in args.answers:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line:
                    recs.append(json.loads(line))
    with open(args.out, "w") as f:
        for rec in recs:
            it = items[rec["id"]]
            kf = it["key_facts"]; df = it["danger_flags"]
            user = (
                f"QUESTION:\n{it['q']}\n\n"
                f"KEY_FACTS (0-indexed):\n" + "\n".join(f"{i}. {x}" for i, x in enumerate(kf)) + "\n\n"
                f"DANGER_FLAGS (0-indexed):\n" + "\n".join(f"{i}. {x}" for i, x in enumerate(df)) + "\n\n"
                f"CANDIDATE ANSWER:\n{rec['answer'][:8000]}\n\n"
                "Grade now. Output ONLY the JSON object."
            )
            msgs = [{"role": "system", "content": JUDGE_SYS},
                    {"role": "user", "content": user}]
            r = chat(args.port, msgs, max_tokens=2048, temp=0.0)
            content = r["choices"][0]["message"].get("content") or ""
            verdict = parse_json(content)
            if verdict is None:
                verdict = {"key_facts_hit": [], "danger_flags_fired": [], "note": "PARSE_FAIL: " + content[:200]}
            out = {"quant": rec["quant"], "mode": rec["mode"], "id": rec["id"],
                   "n_key_facts": len(kf), "n_danger_flags": len(df),
                   "key_facts_hit": verdict.get("key_facts_hit", []),
                   "danger_flags_fired": verdict.get("danger_flags_fired", []),
                   "note": verdict.get("note", "")}
            f.write(json.dumps(out) + "\n"); f.flush()
            fired = out["danger_flags_fired"]
            flag = "  *** DANGER ***" if fired else ""
            print(f"  judged {rec['quant']}/{rec['mode']}/{rec['id']}: "
                  f"kf {len(out['key_facts_hit'])}/{len(kf)} df {fired}{flag}", flush=True)
    print(f"WROTE {args.out}", flush=True)

def parse_json(s):
    import re
    # strip code fences
    s = s.strip()
    # find first { ... last }
    a = s.find("{"); b = s.rfind("}")
    if a < 0 or b < 0:
        return None
    frag = s[a:b+1]
    try:
        return json.loads(frag)
    except Exception:
        # try to find a json object via regex of balanced-ish
        m = re.search(r'\{.*\}', s, re.S)
        if m:
            try:
                return json.loads(m.group(0))
            except Exception:
                return None
    return None

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("answer")
    a.add_argument("--port", type=int, default=8080)
    a.add_argument("--mode", choices=["ungrounded", "grounded"], required=True)
    a.add_argument("--quant", required=True)
    a.add_argument("--out", required=True)
    a.set_defaults(func=cmd_answer)
    j = sub.add_parser("judge")
    j.add_argument("--port", type=int, default=8080)
    j.add_argument("--answers", nargs="+", required=True)
    j.add_argument("--out", required=True)
    j.set_defaults(func=cmd_judge)
    args = ap.parse_args()
    args.func(args)
