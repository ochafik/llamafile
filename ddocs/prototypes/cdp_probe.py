#!/usr/bin/env python3
"""
cdp_probe.py — Prove the Chrome DevTools Protocol (CDP) navigate->extract-text
flow end-to-end, WITHOUT any LLM. This mirrors exactly what a llamafile
`browser_navigate` + `browser_get_text` server_tool pair would do in-process:

  1. posix_spawn headless Chrome with --remote-debugging-port=<port>.
  2. HTTP GET http://127.0.0.1:<port>/json/version  (sanity / wait-for-ready)
  3. HTTP PUT http://127.0.0.1:<port>/json/new?<url>  -> create a target (tab),
     returns JSON incl. "webSocketDebuggerUrl".
  4. Open a WebSocket to that URL; speak CDP JSON ({id,method,params}).
  5. Page.enable; Page.navigate; wait for Page.loadEventFired.
  6. Runtime.evaluate "document.title" and "document.body.innerText".
  7. Print results; clean up (close target, kill Chrome).

Stdlib HTTP + the `websocket-client` package for WS. (In llamafile this maps to
cpp-httplib's Client for HTTP and httplib::ws::WebSocketClient for the WS leg.)
"""
import argparse, json, os, shutil, signal, socket, subprocess, sys, time, urllib.request

PORT = 9333
NAV_URL = "https://example.com"

CHROME_CANDIDATES = [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
    shutil.which("google-chrome"),
    shutil.which("chromium"),
    shutil.which("chromium-browser"),
    shutil.which("chrome"),
]


def find_chrome():
    for c in CHROME_CANDIDATES:
        if c and os.path.exists(c):
            return c
    return None


def http_json(method, url, timeout=2.0):
    req = urllib.request.Request(url, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def wait_for_port(port, deadline):
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


class CDP:
    def __init__(self, ws_url):
        import websocket  # websocket-client
        self.ws = websocket.create_connection(ws_url, timeout=10,
                                               max_size=64 * 1024 * 1024)
        self._id = 0

    def call(self, method, params=None, timeout=10):
        self._id += 1
        mid = self._id
        self.ws.send(json.dumps({"id": mid, "method": method,
                                 "params": params or {}}))
        end = time.time() + timeout
        while time.time() < end:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == mid:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result", {})
            # else: an event; ignore (or buffer)
        raise TimeoutError(method)

    def wait_event(self, method, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            try:
                self.ws.settimeout(max(0.1, end - time.time()))
                msg = json.loads(self.ws.recv())
            except Exception:
                break
            if msg.get("method") == method:
                return msg.get("params", {})
        return None

    def close(self):
        try:
            self.ws.close()
        except Exception:
            pass


def run_flow(ws_url, nav_url=NAV_URL):
    """The researcher's core loop on one CDP target: navigate -> title/text/links."""
    cdp = CDP(ws_url)
    cdp.call("Page.enable")
    cdp.call("Runtime.enable")
    print(f"[navigate] {nav_url}")
    cdp.call("Page.navigate", {"url": nav_url})
    loaded = cdp.wait_event("Page.loadEventFired", timeout=20)
    print(f"[load] loadEventFired={'yes' if loaded else 'TIMEOUT(continuing)'}")
    time.sleep(0.3)

    title = cdp.call("Runtime.evaluate",
                     {"expression": "document.title", "returnByValue": True})
    body = cdp.call("Runtime.evaluate",
                    {"expression": "document.body.innerText", "returnByValue": True})
    links = cdp.call("Runtime.evaluate",
                     {"expression":
                      "JSON.stringify([...document.querySelectorAll('a')]"
                      ".map(a=>({text:a.innerText.trim(),href:a.href}))"
                      ".filter(l=>l.href).slice(0,10))",
                      "returnByValue": True})
    print("\n================ RESULTS ================")
    print("TITLE:", title["result"].get("value"))
    print("BODY innerText:")
    print(body["result"].get("value"))
    print("LINKS:")
    for l in json.loads(links["result"].get("value") or "[]"):
        print(f"  - {l['text']!r} -> {l['href']}")
    print("=========================================\n")
    cdp.close()


def list_targets(port):
    """ATTACH discovery: probe an already-running browser's debug endpoint."""
    return http_json("GET", f"http://127.0.0.1:{port}/json")


def pick_page_target(targets):
    for t in targets:
        if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
            return t
    return None


def spawn_chrome(chrome, port, headless, profile):
    args = [chrome, f"--remote-debugging-port={port}",
            "--no-first-run", "--no-default-browser-check",
            f"--user-data-dir={profile}", "--remote-allow-origins=*", "about:blank"]
    if headless:
        args[1:1] = ["--headless=new", "--disable-gpu"]
    print(f"[spawn] {chrome} --remote-debugging-port={port} "
          f"{'--headless=new' if headless else '(headed)'} --user-data-dir={profile}")
    return subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def mode_launch(headless=True):
    """LAUNCH mode: llamafile spawns a fresh browser, creates a target, runs the flow."""
    chrome = find_chrome()
    if not chrome:
        print("NO CHROME/CHROMIUM FOUND. Fallback: see BROWSER-TOOL-DESIGN.md.")
        return 2
    print(f"[chrome] {chrome}")
    profile = "/tmp/cdp_probe_profile"
    proc = spawn_chrome(chrome, PORT, headless, profile)
    try:
        if not wait_for_port(PORT, time.time() + 15):
            print("FAIL: debug port never opened"); return 1
        ver = http_json("GET", f"http://127.0.0.1:{PORT}/json/version")
        print(f"[cdp] {ver.get('Browser')}  proto={ver.get('Protocol-Version')}")
        try:
            tgt = http_json("PUT", f"http://127.0.0.1:{PORT}/json/new?about:blank")
        except Exception:
            tgt = http_json("GET", f"http://127.0.0.1:{PORT}/json/new?about:blank")
        print(f"[target] NEW id={tgt['id']}")
        run_flow(tgt["webSocketDebuggerUrl"])
        print("OK: LAUNCH-mode CDP navigate -> extract-text flow validated.")
        return 0
    finally:
        proc.send_signal(signal.SIGTERM)
        try: proc.wait(timeout=5)
        except Exception: proc.kill()


def mode_attach(port, spawn_stand_in=True):
    """ATTACH mode: connect to an ALREADY-RUNNING browser's debug endpoint.

    Demonstrates exactly what llamafile does when --browser-endpoint is set:
    probe /json, reuse an EXISTING target (the user's tab), drive it in-session.
    If nothing is listening on `port` and spawn_stand_in is set, we first launch a
    stand-in Chrome to play 'the user's already-running browser', then attach to it
    as a separate, independent CDP connection (no relaunch)."""
    stand_in = None
    if not wait_for_port(port, time.time() + 0.5):
        if not spawn_stand_in:
            print(f"ATTACH: nothing listening on 127.0.0.1:{port}. "
                  f"Start Chrome with --remote-debugging-port={port} first.")
            return 2
        chrome = find_chrome()
        if not chrome:
            print("NO CHROME/CHROMIUM FOUND. Fallback: see BROWSER-TOOL-DESIGN.md.")
            return 2
        print(f"[attach] no browser on :{port}; launching a STAND-IN to represent "
              f"'the user's already-running browser'")
        stand_in = spawn_chrome(chrome, port, headless=True,
                                profile="/tmp/cdp_probe_attach_profile")
        if not wait_for_port(port, time.time() + 15):
            print("FAIL: stand-in debug port never opened"); return 1
    try:
        ver = http_json("GET", f"http://127.0.0.1:{port}/json/version")
        print(f"[attach] connected to RUNNING browser: {ver.get('Browser')}  "
              f"proto={ver.get('Protocol-Version')}")
        targets = list_targets(port)
        print(f"[attach] discovered {len(targets)} existing target(s):")
        for t in targets[:5]:
            print(f"    - type={t.get('type')} title={t.get('title')!r} "
                  f"url={t.get('url')}")
        tgt = pick_page_target(targets)
        if not tgt:
            print("ATTACH: no existing page target to drive."); return 1
        print(f"[attach] REUSING existing target id={tgt['id']} "
              f"(NOT creating a new tab) -> drives the user's live session")
        run_flow(tgt["webSocketDebuggerUrl"])
        print("OK: ATTACH-mode CDP flow validated against an already-running browser.")
        return 0
    finally:
        if stand_in is not None:
            stand_in.send_signal(signal.SIGTERM)
            try: stand_in.wait(timeout=5)
            except Exception: stand_in.kill()


def main():
    ap = argparse.ArgumentParser(description="CDP probe: LAUNCH + ATTACH modes")
    ap.add_argument("--mode", choices=["launch", "attach", "both"], default="both")
    ap.add_argument("--headed", action="store_true", help="launch mode: headed window")
    ap.add_argument("--attach-port", type=int, default=9222,
                    help="ATTACH to a browser already on this debug port")
    a = ap.parse_args()
    rc = 0
    if a.mode in ("launch", "both"):
        print("\n########## MODE 1: LAUNCH (llamafile spawns a fresh browser) ##########")
        rc |= mode_launch(headless=not a.headed)
    if a.mode in ("attach", "both"):
        print("\n########## MODE 2: ATTACH (connect to an already-running browser) #####")
        rc |= mode_attach(a.attach_port)
    return rc


if __name__ == "__main__":
    sys.exit(main())
