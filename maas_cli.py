#!/usr/bin/env python3
"""MAAS Interactive Client CLI
Run scripts, check node specs, and read logs on your remote MAAS node.
"""

import sys
import os
import json
import time
import argparse
import urllib.request
import urllib.error

DEFAULT_URL = os.environ.get("MAAS_URL", "http://10.80.18.195:7420")
DEFAULT_TOKEN = os.environ.get("MAAS_TOKEN", "maas_qza8ysbyzpjn2xzt9pf3qjm2zp85a9wveda4ygan")

class MaasClient:
    def __init__(self, base_url=DEFAULT_URL, token=DEFAULT_TOKEN):
        self.base_url = base_url.rstrip("/")
        self.token = token
        self._counter = 0

    def mcp_call(self, name, arguments=None):
        self._counter += 1
        payload = {
            "jsonrpc": "2.0",
            "id": self._counter,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments or {}}
        }
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            f"{self.base_url}/mcp",
            data=data,
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.token}"}
        )
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                res = json.loads(resp.read().decode("utf-8"))
                result = res.get("result", {})
                content = result.get("content", [])
                text = content[0].get("text", "") if content else ""
                # Parse JSON if output is serialized
                if "\n{" in text:
                    prefix, json_str = text.split("\n", 1)
                    try:
                        return result.get("isError", False), json.loads(json_str), text
                    except Exception:
                        pass
                return result.get("isError", False), text, text
        except urllib.error.HTTPError as e:
            return True, f"HTTP Error {e.code}: {e.read().decode()}", ""
        except Exception as e:
            return True, str(e), ""

    def get_capabilities(self):
        err, data, raw = self.mcp_call("get_node_capabilities")
        if err:
            print("Error:", data)
            return
        if isinstance(data, dict):
            print(f"=== Node: {data.get('node', 'maas-node')} ===")
            print(f"OS: {data.get('os_pretty')} ({data.get('arch')})")
            cpu = data.get("cpu", {})
            print(f"CPU: {cpu.get('model')} ({cpu.get('cores')} cores, AVX2={cpu.get('avx2')})")
            mem = data.get("memory", {})
            print(f"RAM: {mem.get('available_mb')} MB free / {mem.get('total_mb')} MB total")
            disk = data.get("disk", {})
            print(f"Disk: {disk.get('free_mb')} MB free")
            print("Runtimes:")
            for name, info in data.get("runtimes", {}).items():
                print(f"  • {name}: {info.get('version', '')} ({info.get('path')})")
        else:
            print(raw)

    def run_script(self, code_or_file, explanation="Run via MAAS CLI", interpreter=None):
        if os.path.isfile(code_or_file):
            filename = os.path.basename(code_or_file)
            with open(code_or_file, "r") as f:
                code = f.read()
            print(f"Uploading script '{filename}' to node workspace...")
            err, resp, _ = self.mcp_call("write_file", {"path": filename, "content": code})
            if err:
                print("Failed to upload file:", resp)
                return
            target_path = filename
            code_payload = None
        else:
            target_path = None
            code_payload = code_or_file

        args = {
            "explanation": explanation,
            "wait_sec": 55,
            "wait_for_exit_sec": 60
        }
        if target_path:
            args["path"] = target_path
        else:
            args["code"] = code_payload
        if interpreter:
            args["interpreter"] = interpreter

        print("\n" + "=" * 60)
        print("🔔 APPROVAL REQUIRED ON YOUR PHONE OR HP TERMINAL!")
        print("=" * 60)
        print(f"AI/Script Explanation: {explanation}")
        print("Waiting for approval (tap Run on phone or press 'y' on HP laptop)...")

        err, data, raw = self.mcp_call("execute_script", args)
        if isinstance(data, dict):
            task_id = data.get("task_id", "")
            state = data.get("state", "")
            failure_kind = data.get("failure_kind", "")
            print(f"\nTask {task_id}: {state} (result: {failure_kind})")

            # Fetch process logs
            err_log, logs, raw_logs = self.mcp_call("read_process_logs", {"task_id": task_id, "tail": True, "max_bytes": 8192})
            print("\n--- Output ---")
            if isinstance(raw_logs, str):
                print(raw_logs)
            else:
                print(logs)
            print("--------------")
        else:
            print("Response:", raw)

def main():
    parser = argparse.ArgumentParser(description="MAAS Remote CLI")
    subparsers = parser.add_subparsers(dest="cmd")

    subparsers.add_parser("specs", help="Show remote HP node specs and runtimes")

    run_parser = subparsers.add_parser("run", help="Run a script or inline code on the node")
    run_parser.add_argument("target", help="Path to file or inline code string")
    run_parser.add_argument("-e", "--explanation", default="Executed via MAAS CLI", help="Explanation for approval")
    run_parser.add_argument("-i", "--interpreter", help="Override interpreter (python, node, bash, sh)")

    args = parser.parse_args()
    client = MaasClient()

    if args.cmd == "specs" or not args.cmd:
        client.get_capabilities()
    elif args.cmd == "run":
        client.run_script(args.target, explanation=args.explanation, interpreter=args.interpreter)

if __name__ == "__main__":
    main()
