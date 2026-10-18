#!/usr/bin/env python3
"""MAAS Ollama Agent
Connects your local Ollama model (gemma4:12b) to your remote MAAS node.
This turns Gemma into an autonomous coding/research agent that can inspect,
write code, run scripts, and query datasets on your HP Linux server.
"""

import sys
import os
import json
import urllib.request
import urllib.error

OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://localhost:11434/api/chat")
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "gemma4:12b")
MAAS_URL = os.environ.get("MAAS_URL", "http://10.80.18.195:7420")
MAAS_TOKEN = os.environ.get("MAAS_TOKEN", "maas_qza8ysbyzpjn2xzt9pf3qjm2zp85a9wveda4ygan")

class MaasAgent:
    def __init__(self, maas_url=MAAS_URL, token=MAAS_TOKEN, model=OLLAMA_MODEL):
        self.maas_url = maas_url.rstrip("/")
        self.token = token
        self.model = model
        self.tools = []
        self.ollama_tools = []
        self._mcp_id = 0

    def mcp_post(self, method, params=None):
        self._mcp_id += 1
        payload = {
            "jsonrpc": "2.0",
            "id": self._mcp_id,
            "method": method,
            "params": params or {}
        }
        req = urllib.request.Request(
            f"{self.maas_url}/mcp",
            data=json.dumps(payload).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.token}"
            }
        )
        with urllib.request.urlopen(req, timeout=120) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def load_tools(self):
        print(f"📡 Fetching tools from MAAS node at {self.maas_url}...")
        resp = self.mcp_post("tools/list")
        self.tools = resp.get("result", {}).get("tools", [])
        self.ollama_tools = []
        for t in self.tools:
            self.ollama_tools.append({
                "type": "function",
                "function": {
                    "name": t["name"],
                    "description": t.get("description", ""),
                    "parameters": t.get("inputSchema", {})
                }
            })
        print(f"✓ Loaded {len(self.ollama_tools)} tools from MAAS node into Gemma.\n")

    def execute_tool(self, name, arguments):
        print(f"\n  ⚙️  [Calling MAAS Tool]: {name}")
        if arguments:
            print(f"     Args: {json.dumps(arguments, indent=2)}")
        if name in ["execute_script", "install_packages", "ingest_dataset"]:
            print("  🔔 Waiting for approval on your Phone or HP Laptop terminal...")

        resp = self.mcp_post("tools/call", {"name": name, "arguments": arguments})
        result = resp.get("result", {})
        content = result.get("content", [])
        output_text = "\n".join([c.get("text", "") for c in content if "text" in c])
        
        # If executing script, fetch logs if not fully printed
        if name == "execute_script" and not result.get("isError"):
            try:
                task_info = json.loads(output_text.split("\n", 1)[-1])
                task_id = task_info.get("task_id")
                if task_id:
                    log_resp = self.mcp_post("tools/call", {
                        "name": "read_process_logs",
                        "arguments": {"task_id": task_id, "tail": True, "max_bytes": 4096}
                    })
                    log_content = log_resp.get("result", {}).get("content", [])
                    log_text = "\n".join([c.get("text", "") for c in log_content if "text" in c])
                    output_text += f"\n\n[Process Output]:\n{log_text}"
            except Exception:
                pass

        print(f"  ✓ [Tool Finished]: {name} (Error: {result.get('isError', False)})\n")
        return output_text

    def chat_turn(self, messages):
        payload = {
            "model": self.model,
            "messages": messages,
            "tools": self.ollama_tools,
            "stream": False
        }
        req = urllib.request.Request(
            OLLAMA_URL,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                return data.get("message", {})
        except Exception as e:
            print(f"Ollama chat error: {e}")
            return {"role": "assistant", "content": f"Error communicating with Ollama: {e}"}

    def run_loop(self):
        self.load_tools()
        system_prompt = (
            "You are an AI assistant powered by Gemma and connected to an external Linux compute server "
            "called 'maas-node' via MAAS (Make Anything Server).\n"
            "You have access to 16 remote tools to interact with the Linux server:\n"
            "- get_node_capabilities: Check CPU, memory, OS, and runtimes.\n"
            "- write_file, read_file, list_files: Manage files in the node's workspace.\n"
            "- execute_script: Run scripts on the node with memory/time safety.\n"
            "- read_process_logs: View output logs of tasks.\n"
            "- ingest_dataset, query_sql: Load large CSV/TSV data into SQLite and run read-only SQL queries.\n\n"
            "When asked to perform tasks on the computer, USE YOUR TOOLS. When executing scripts, provide a clear explanation."
        )
        messages = [{"role": "system", "content": system_prompt}]

        print("=" * 65)
        print(f"🤖 MAAS Ollama Agent ({self.model}) Ready!")
        print("Connected to: HP Linux Server (http://10.80.18.195:7420)")
        print("Type your request below. Type 'exit' or 'quit' to quit.")
        print("=" * 65 + "\n")

        while True:
            try:
                user_input = input("You > ").strip()
            except (EOFError, KeyboardInterrupt):
                print("\nGoodbye!")
                break

            if not user_input:
                continue
            if user_input.lower() in ["exit", "quit"]:
                break

            messages.append({"role": "user", "content": user_input})

            # Agent loop: allow multiple consecutive tool calls
            while True:
                print(f"🤔 Gemma ({self.model}) is thinking...")
                assistant_msg = self.chat_turn(messages)
                messages.append(assistant_msg)

                tool_calls = assistant_msg.get("tool_calls", [])
                if not tool_calls:
                    # Model responded with final text
                    content = assistant_msg.get("content", "")
                    print(f"\n🤖 Gemma:\n{content}\n")
                    break

                for tc in tool_calls:
                    func = tc.get("function", {})
                    fn_name = func.get("name")
                    fn_args = func.get("arguments", {})
                    tc_id = tc.get("id")
                    output = self.execute_tool(fn_name, fn_args)
                    tool_msg = {
                        "role": "tool",
                        "content": str(output)
                    }
                    if tc_id:
                        tool_msg["tool_call_id"] = tc_id
                    messages.append(tool_msg)

def main():
    agent = MaasAgent()
    agent.run_loop()

if __name__ == "__main__":
    main()
