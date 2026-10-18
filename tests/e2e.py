#!/usr/bin/env python3
"""End-to-end test against a running MAAS node. Standard library only.

Usage: python3 tests/e2e.py http://127.0.0.1:7420 <client-code> <phone-code>
(The codes are printed by `maas start` or `maas pair`.)

Checks: pairing, auth rejection, MCP initialize/tools/list, capability probe,
write_file + execute_script with a phone approval, hash-bound re-approval,
MODULE_NOT_FOUND and OOM_KILLED classification, log paging, path jail,
dataset ingest + read-only query_sql.
"""
import json
import sys
import threading
import time
import urllib.error
import urllib.request

BASE, CLIENT_CODE, PHONE_CODE = sys.argv[1].rstrip("/"), sys.argv[2], sys.argv[3]
failures = []
OVERWRITE = len(sys.argv) > 4 and sys.argv[4] == "rerun"


def http(method, path, body=None, token=None, raw=None, headers=None):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    request = urllib.request.Request(BASE + path, data=data, method=method)
    request.add_header("Content-Type", "application/json")
    if token:
        request.add_header("Authorization", "Bearer " + token)
    for key, value in (headers or {}).items():
        request.add_header(key, value)
    try:
        with urllib.request.urlopen(request, timeout=150) as response:
            text = response.read().decode()
            return response.status, (json.loads(text) if text else None)
    except urllib.error.HTTPError as error:
        text = error.read().decode()
        return error.code, (json.loads(text) if text else None)


def check(name, condition, detail=""):
    print(("PASS " if condition else "FAIL ") + name + ("" if condition else "  -> " + str(detail)[:400]))
    if not condition:
        failures.append(name)


counter = [0]


def mcp(token, method, params=None):
    counter[0] += 1
    status, reply = http("POST", "/mcp", {"jsonrpc": "2.0", "id": counter[0], "method": method, "params": params or {}}, token)
    return status, reply


def tool(token, name, arguments):
    status, reply = mcp(token, "tools/call", {"name": name, "arguments": arguments})
    result = reply["result"]
    data = json.loads(result["content"][0]["text"].split("\n", 1)[1])
    return result["isError"], data, result


def approve_when_pending(phone_token, decision="approve", timeout=20):
    """Plays the human with a phone: approves the first pending item."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        status, data = http("GET", "/api/status", token=phone_token)
        if data and data.get("pending"):
            item = data["pending"][0]
            http("POST", "/api/approvals/" + item["id"], {"decision": decision}, phone_token)
            return item
        time.sleep(0.3)
    return None


def run_with_approval(client, phone, arguments, decision="approve"):
    seen = {}
    thread = threading.Thread(target=lambda: seen.update(item=approve_when_pending(phone, decision)))
    thread.start()
    is_error, data, _ = tool(client, "execute_script", arguments)
    thread.join()
    return is_error, data, seen.get("item")


# ---- pairing and auth
status, reply = http("POST", "/api/pair", {"code": CLIENT_CODE, "label": "e2e client"})
check("client pairs with one-time code", status == 200 and reply.get("role") == "client", reply)
client = reply["token"]
status, _ = http("POST", "/api/pair", {"code": CLIENT_CODE})
check("code cannot be reused", status == 403)
status, reply = http("POST", "/api/pair", {"code": PHONE_CODE, "label": "e2e phone"})
check("phone pairs as approver", status == 200 and reply.get("role") == "approver", reply)
phone = reply["token"]
status, _ = mcp(None, "tools/list")
check("MCP without token is 401", status == 401)
status, _ = mcp("maas_wrongtokenwrongtokenwrong", "tools/list")
check("MCP with bad token is 401", status == 401)
status, _ = mcp(phone, "tools/list")
check("phone token cannot use MCP", status == 401)
status, _ = http("POST", "/api/approvals/t_x", {"decision": "approve"}, client)
check("AI client token cannot approve", status == 403)

# ---- MCP basics
status, reply = mcp(client, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                           "clientInfo": {"name": "e2e", "version": "1"}})
check("initialize", status == 200 and reply["result"]["protocolVersion"] == "2025-06-18", reply)
status, reply = http("POST", "/mcp", {"jsonrpc": "2.0", "method": "notifications/initialized"}, client)
check("notification gets 202", status == 202)
status, reply = mcp(client, "tools/list")
names = [t["name"] for t in reply["result"]["tools"]]
check("tools/list has 16 tools", len(names) == 16 and "execute_script" in names, names)
time.sleep(1)
is_error, caps, _ = tool(client, "get_node_capabilities", {})
check("capabilities report CPU, RAM, python", not is_error and caps["memory"]["total_mb"] > 0 and "python" in caps["runtimes"], caps.get("runtimes"))

# ---- path jail
is_error, data, _ = tool(client, "write_file", {"path": "../escape.txt", "content": "x"})
check("path jail blocks ..", is_error and data["error_kind"] == "BAD_PATH", data)
is_error, data, _ = tool(client, "read_file", {"path": "/etc/passwd"})
check("path jail blocks absolute paths", is_error, data)

# ---- run a script with phone approval
is_error, data, _ = tool(client, "write_file", {"path": "hello.py", "content": "import sys\nprint('hello from the node', sys.argv[1:])\n"})
check("write_file", not is_error and data["sha256"], data)
is_error, task, item = run_with_approval(client, phone, {"path": "hello.py", "args": ["a", "b"],
                                                          "explanation": "prints hello", "wait_for_exit_sec": 20})
check("approval prompt shows exact bytes", item is not None and "hello from the node" in item.get("content", "") and item.get("ai_says") == "prints hello", item)
check("script ran OK after approval", task.get("state") == "EXITED" and task.get("failure_kind") == "OK", task)
is_error, logs, raw = tool(client, "read_process_logs", {"task_id": task["task_id"]})
log_text = raw["content"][1]["text"] if len(raw["content"]) > 1 else ""
check("log contains output", "hello from the node ['a', 'b']" in log_text, log_text)

# ---- denial
is_error, task, item = run_with_approval(client, phone, {"code": "print('should not run')", "wait_sec": 20}, decision="deny")
check("denied script does not run", task.get("state") == "DENIED", task)

# ---- changed script needs a new approval and shows a diff
tool(client, "write_file", {"path": "hello.py", "content": "import sys\nprint('hello again')\n"})
is_error, task, item = run_with_approval(client, phone, {"path": "hello.py"})
check("edited script asks again and shows a diff", item is not None and "+ print('hello again')" in item.get("diff", ""), item)

# ---- failure taxonomy
tool(client, "write_file", {"path": "needs_module.py", "content": "import definitely_not_installed_pkg\n"})
is_error, task, _ = run_with_approval(client, phone, {"path": "needs_module.py"})
result = task.get("result", {})
check("MODULE_NOT_FOUND with module name", task.get("failure_kind") == "MODULE_NOT_FOUND" and result.get("module") == "definitely_not_installed_pkg", task)
tool(client, "write_file", {"path": "syntax.py", "content": "print('ok')\ndef broken(:\n"})
is_error, task, _ = run_with_approval(client, phone, {"path": "syntax.py"})
check("SYNTAX_ERROR with line", task.get("failure_kind") == "SYNTAX_ERROR" and task.get("result", {}).get("line") == 2, task)

# ---- memory guardrail: the job is killed, the node stays up
hog = "import time\nblocks = []\nwhile True:\n    blocks.append(bytearray(50 * 1024 * 1024))\n    time.sleep(0.05)\n"
tool(client, "write_file", {"path": "hog.py", "content": hog})
is_error, task, _ = run_with_approval(client, phone, {"path": "hog.py", "memory_mb": 300, "wait_for_exit_sec": 40})
check("memory hog is OOM_KILLED", task.get("failure_kind") == "OOM_KILLED", task)
status, health = http("GET", "/health")
check("node still alive after OOM kill", status == 200 and health.get("ok"))

# ---- timeout + terminate
is_error, task, _ = run_with_approval(client, phone, {"code": "import time\ntime.sleep(60)\n", "timeout_sec": 3, "wait_for_exit_sec": 20})
check("TIMEOUT enforced", task.get("failure_kind") == "TIMEOUT", task)

# ---- data: upload + ingest + query
csv = "id,name,score,zip\n1,ann,9.5,00123\n2,bob,NA,10001\n3,cy,7,\n4,dee,8.25,22222,extra\n"
status, reply = http("PUT", "/api/upload?name=small.csv&offset=0&total=%d" % len(csv), raw=csv.encode(), token=client,
                     headers={"Content-Type": "application/octet-stream"})
status2, reply2 = http("POST", "/api/upload/complete?name=small.csv&total=%d&overwrite=1" % len(csv), {}, client)
check("resumable upload", status == 200 and status2 == 200 and reply2.get("path") == "uploads/small.csv", (reply, reply2))
if OVERWRITE:  # replacing an existing dataset is a Tier 2 action: needs approval
    t = threading.Thread(target=approve_when_pending, args=(phone,)); t.start()
is_error, task, _ = tool(client, "ingest_dataset", {"path": "uploads/small.csv", "wait_sec": 20, "overwrite": OVERWRITE})
if OVERWRITE:
    t.join()
check("ingest finished", task.get("failure_kind") == "OK", task)
is_error, profile, _ = tool(client, "describe_dataset", {"name": "small"})
columns = {c["name"]: c for c in profile["profile"]["columns"]}
check("profile: 4 rows, nulls counted, ragged row reported",
      profile["profile"]["rows"] == 4 and columns["score"]["nulls"] == 1
      and columns["zip"]["type"].startswith("mixed") and profile["profile"]["issues"]["long_rows"] == 1, profile)
is_error, rows, _ = tool(client, "query_sql", {"dataset": "small", "sql": "SELECT zip, typeof(zip) FROM data WHERE id = 1"})
check("leading-zero code kept as text", rows["rows"] == [["00123", "text"]], rows)
is_error, rows, _ = tool(client, "query_sql", {"dataset": "small", "sql": "SELECT name, score FROM data WHERE score > 8 ORDER BY score"})
check("query_sql works", not is_error and rows["rows"] == [["dee", 8.25], ["ann", 9.5]], rows)
is_error, data, _ = tool(client, "query_sql", {"dataset": "small", "sql": "DELETE FROM data"})
check("query_sql is read-only", is_error and data["error_kind"] == "READ_ONLY", data)

print()
print("ALL PASSED" if not failures else "FAILED: " + ", ".join(failures))
sys.exit(1 if failures else 0)
