#!/usr/bin/env python3
"""MAAS Stdio-to-HTTP MCP Bridge
Connects stdio-based MCP clients (Antigravity, Claude Desktop, Cursor, VS Code)
to a remote MAAS node over HTTP.
"""

import sys
import os
import json
import urllib.request
import urllib.error

DEFAULT_URL = os.environ.get("MAAS_URL", "http://10.80.18.195:7420/mcp")
DEFAULT_TOKEN = os.environ.get("MAAS_TOKEN", "maas_qza8ysbyzpjn2xzt9pf3qjm2zp85a9wveda4ygan")

def main():
    url = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL
    token = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_TOKEN

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req_obj = json.loads(line)
        except Exception as e:
            sys.stderr.write(f"Bridge JSON parse error: {e}\n")
            sys.stderr.flush()
            continue

        req_id = req_obj.get("id")
        req_data = line.encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {token}"
        }
        http_req = urllib.request.Request(url, data=req_data, headers=headers)
        try:
            with urllib.request.urlopen(http_req, timeout=120) as resp:
                resp_bytes = resp.read()
                if resp_bytes and req_id is not None:
                    sys.stdout.write(resp_bytes.decode("utf-8") + "\n")
                    sys.stdout.flush()
        except urllib.error.HTTPError as e:
            err_body = e.read().decode("utf-8", errors="replace")
            sys.stderr.write(f"HTTP error {e.code}: {err_body}\n")
            sys.stderr.flush()
            if req_id is not None:
                err_resp = {
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "error": {"code": -32603, "message": f"HTTP {e.code}: {err_body}"}
                }
                sys.stdout.write(json.dumps(err_resp) + "\n")
                sys.stdout.flush()
        except Exception as e:
            sys.stderr.write(f"Bridge request error: {e}\n")
            sys.stderr.flush()
            if req_id is not None:
                err_resp = {
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "error": {"code": -32603, "message": str(e)}
                }
                sys.stdout.write(json.dumps(err_resp) + "\n")
                sys.stdout.flush()

if __name__ == "__main__":
    main()
