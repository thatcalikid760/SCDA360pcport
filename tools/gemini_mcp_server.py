"""Minimal stdio MCP server exposing consult_gemini. Stdlib only.

Routing: GEMINI_API_KEY (env or .env) -> Gemini REST API; else `gemini` CLI on PATH.
Every exchange is appended to BRIDGE_LOG.md in the project root.
"""
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "BRIDGE_LOG.md")
# gemini-2.5-pro returns 404 for new API keys; override with GEMINI_MODEL.
MODEL = os.environ.get("GEMINI_MODEL", "gemini-3.1-pro-preview")
# Free-tier keys get 0 Pro quota (429); fall back to Flash. 503 = Google overload, retried.
FALLBACK = os.environ.get("GEMINI_FALLBACK_MODEL", "gemini-3.8-flash")
API = "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent"
MAX_FILE = 200_000  # bytes per context file
SYSTEM = (
    "You are Co-Architect on a static recompilation of Splinter Cell: Double Agent "
    "(Xbox 360, title 555307D7) to native Windows x64. Domain: XEX2 format, PowerPC "
    "(Xenon, VMX128), Xenos GPU, XAM/xboxkrnl HLE, XenonRecomp/ReXGlue-style pipelines. "
    "Caveman mode: ultra-terse, no fluff. Output only actionable steps, code, hex "
    "addresses, diffs. Ponytail mode: smallest change that solves it. Flag uncertainty."
)

PROTOCOL = "2024-11-05"
TOOLS = [{
    "name": "consult_gemini",
    "description": "Ask Gemini (Xbox 360 recomp co-architect) a question. "
                   "context_files: project-relative paths whose contents are attached.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "task": {"type": "string"},
            "context_files": {"type": "array", "items": {"type": "string"}},
        },
        "required": ["task"],
    },
}]


def load_key():
    if os.environ.get("GEMINI_API_KEY"):
        return os.environ["GEMINI_API_KEY"]
    env = os.path.join(ROOT, ".env")
    if os.path.isfile(env):
        with open(env, encoding="utf-8") as f:
            for line in f:
                k, _, v = line.strip().partition("=")
                if k.strip() == "GEMINI_API_KEY" and v.strip():
                    return v.strip().strip('"').strip("'")
    return None


def read_context(paths):
    # Only files inside the project root are sent off-machine.
    parts = []
    for p in paths or []:
        full = os.path.realpath(os.path.join(ROOT, p))
        if os.path.commonpath([full, os.path.realpath(ROOT)]) != os.path.realpath(ROOT):
            parts.append("### %s\n[refused: outside project root]" % p)
            continue
        try:
            with open(full, "rb") as f:
                data = f.read(MAX_FILE + 1)
        except OSError as e:
            parts.append("### %s\n[unreadable: %s]" % (p, e))
            continue
        text = data[:MAX_FILE].decode("utf-8", "replace")
        if len(data) > MAX_FILE:
            text += "\n[truncated at %d bytes]" % MAX_FILE
        parts.append("### %s\n```\n%s\n```" % (p, text))
    return "\n\n".join(parts)


def via_api(key, prompt):
    err = None
    for model in dict.fromkeys([MODEL, FALLBACK]):
        for attempt in range(3):
            try:
                return model, call_api(key, model, prompt)
            except urllib.error.HTTPError as e:
                err = "Gemini API %s HTTP %d: %s" % (model, e.code, e.read()[:300].decode("utf-8", "replace"))
                if e.code == 503:
                    time.sleep(5 * (attempt + 1))
                    continue
                if e.code in (404, 429):
                    break  # try next model
                raise RuntimeError(err)
    raise RuntimeError(err)


def call_api(key, model, prompt):
    body = json.dumps({
        "systemInstruction": {"parts": [{"text": SYSTEM}]},
        "contents": [{"role": "user", "parts": [{"text": prompt}]}],
    }).encode()
    req = urllib.request.Request(API % model, data=body, headers={
        "Content-Type": "application/json", "x-goog-api-key": key})
    with urllib.request.urlopen(req, timeout=300) as r:
        out = json.load(r)
    cands = out.get("candidates") or []
    if not cands:
        raise RuntimeError("Gemini API returned no candidates: %s" % json.dumps(out)[:500])
    return "".join(p.get("text", "") for p in cands[0].get("content", {}).get("parts", []))


def find_cli():
    exts = os.environ.get("PATHEXT", "").split(os.pathsep) if os.name == "nt" else [""]
    for d in os.environ.get("PATH", "").split(os.pathsep):
        for ext in [""] + exts:
            cand = os.path.join(d, "gemini" + ext)
            if os.path.isfile(cand) and (os.name == "nt" and ext or os.access(cand, os.X_OK)):
                return cand
    return None


def via_cli(cli, prompt):
    # Full prompt goes over stdin; the CLI appends -p to stdin input. Keeps argv fixed (no quoting/length issues).
    r = subprocess.run([cli, "-p", "Answer the request above."], input=SYSTEM + "\n\n" + prompt,
                       capture_output=True, text=True, encoding="utf-8", timeout=600, cwd=ROOT)
    if r.returncode != 0:
        raise RuntimeError("gemini CLI exit %d: %s" % (r.returncode, r.stderr[-500:]))
    return r.stdout


def log(task, files, route, reply):
    one = lambda s, n: " ".join(s.split())[:n]
    with open(LOG, "a", encoding="utf-8") as f:
        f.write("\n## %s via %s\n- **Q:** %s\n- **Files:** %s\n- **A:** %s\n" % (
            time.strftime("%Y-%m-%d %H:%M:%S"), route, one(task, 300),
            ", ".join(files or []) or "-", one(reply, 600)))


def consult(args):
    task = args.get("task", "")
    files = args.get("context_files") or []
    ctx = read_context(files)
    prompt = task + ("\n\n## Context files\n" + ctx if ctx else "")
    key = load_key()
    if key:
        model, reply = via_api(key, prompt)
        route = "api:" + model
    else:
        cli = find_cli()
        if not cli:
            raise RuntimeError("No GEMINI_API_KEY (env or .env) and no `gemini` CLI on PATH.")
        route, reply = "cli", via_cli(cli, prompt)
    log(task, files, route, reply)
    return reply


def handle(msg):
    method, mid = msg.get("method"), msg.get("id")
    if method == "initialize":
        ver = msg.get("params", {}).get("protocolVersion", PROTOCOL)
        result = {"protocolVersion": ver, "capabilities": {"tools": {}},
                  "serverInfo": {"name": "gemini-coarchitect", "version": "1.0.0"}}
    elif method == "tools/list":
        result = {"tools": TOOLS}
    elif method == "tools/call":
        p = msg.get("params", {})
        if p.get("name") != "consult_gemini":
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "unknown tool"}}
        try:
            result = {"content": [{"type": "text", "text": consult(p.get("arguments") or {})}]}
        except Exception as e:
            log(p.get("arguments", {}).get("task", ""), p.get("arguments", {}).get("context_files"), "error", str(e))
            result = {"content": [{"type": "text", "text": "ERROR: %s" % e}], "isError": True}
    elif method == "ping":
        result = {}
    elif mid is None:
        return None  # notification (e.g. notifications/initialized)
    else:
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}}
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            resp = handle(json.loads(line))
        except json.JSONDecodeError:
            resp = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse error"}}
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    # Direct use: python tools/gemini_mcp_server.py --cli "<query>" [context_file ...]
    if len(sys.argv) > 2 and sys.argv[1] == "--cli":
        try:
            print(consult({"task": sys.argv[2], "context_files": sys.argv[3:]}))
        except Exception as e:
            log(sys.argv[2], sys.argv[3:], "error", str(e))
            sys.exit("ERROR: %s" % e)
    else:
        main()
