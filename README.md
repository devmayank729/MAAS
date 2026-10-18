# MAAS — Make Anything Server

> **Turn any spare computer into an AI-operable research node with one command, a QR code, and your phone as the safety switch.**

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/Language-C%2B%2B17-blue.svg)](src/)
[![MCP Compliant](https://img.shields.io/badge/MCP-Streamable%20HTTP%20%26%20Stdio-orange.svg)](https://modelcontextprotocol.io)
[![Ollama Ready](https://img.shields.io/badge/Ollama-Gemma%204%20Agent-purple.svg)](ollama_agent.py)
[![Hackathon](https://img.shields.io/badge/BMSCE%20IEEE-Hacktoberfest%202026-red.svg)](#)

---

## 💡 The Idea: What is MAAS?

Modern AI agents (Claude Code, local Ollama models like Gemma 4, Cursor, Antigravity) are powerful coding partners: they write scripts, install packages, ingest datasets, and iterate on code. 

However, running arbitrary agent-generated code on your primary workstation presents serious problems:
1. **Security & Data Risk**: Prompt injection or hallucinations can accidentally delete files or expose private tokens.
2. **Resource Exhaustion**: Large data processing, compiling, or infinite loops freeze your work machine and drain your battery.
3. **Expensive Cloud Lock-in**: Spinning up cloud VMs (AWS EC2, GCP, modal) costs money and adds complex SSH/VPN network setup.

**MAAS solves this by turning old, spare laptops or desktop computers into secure, dedicated compute nodes over your local Wi-Fi.**

Built as a lightweight, single-binary C++17 daemon with zero runtime dependencies, MAAS serves an **MCP (Model Context Protocol)** endpoint. AI agents on your main machine can offload heavy compute, file manipulation, and dataset analysis to the spare machine—while **you retain 100% human-in-the-loop control through your phone**.

---

## 📐 Architecture

```
                       ┌───────────────────────────────────────────────┐
                       │          Client Machine (Main Laptop)         │
                       │  • Local Ollama (Gemma 4) Agent (Python)      │
                       │  • Claude Code CLI                            │
                       │  • Cursor / Antigravity / Claude Desktop      │
                       └───────────────────────┬───────────────────────┘
                                               │
                                               │ Model Context Protocol
                                               │ (HTTP POST /mcp or Stdio Bridge)
                                               ▼
                       ┌───────────────────────────────────────────────┐
                       │           MAAS Node (Spare Computer)          │
                       │             Single C++17 Daemon               │
                       │                                               │
                       │   ┌───────────────────────────────────────┐   │
                       │   │ 16 MCP Tools (Execution, Data, Files) │   │
                       │   └───────────────────┬───────────────────┘   │
                       │                       │                       │
                       │            Hash-Bound Approval Check          │
                       │                       │                       │
                       │         ┌─────────────┴─────────────┐         │
                       │         ▼                           ▼         │
                       │   [ Tier 0 / 1 ]             [ Tier 2 ]       │
                       │   • Read hardware specs      • Execute script │
                       │   • Workspace files          • Pip install    │
                       │   • Read process logs        • Ingest dataset │
                       │         │                           │         │
                       │         ▼                           │ (Paused)│
                       │    (Immediate)                      ▼         │
                       │                             Waiting for Human │
                       └─────────────────────────────────────┬─────────┘
                                                             │
                                               Push Alert via│Local Wi-Fi
                                               LAN / QR Link │
                                                             ▼
                                               ┌───────────────────────┐
                                               │   Phone Safety Switch │
                                               │ • See exact bytes     │
                                               │ • See "AI says: ..."  │
                                               │ • Tap [Run] / [Deny]  │
                                               └───────────────────────┘
```

---

## ✨ Key Features & Innovations

### 1. 🛡️ Phone as the Hardware Safety Switch
Every script execution, package installation, or dataset overwrite requires physical human approval. 
- Scan the terminal QR code with your phone camera.
- The phone loads an ultra-fast, zero-dependency safety page embedded directly in the C++ binary.
- View the **exact script contents**, a **diff against prior runs**, and the **AI's stated reasoning** ("AI says: ...").
- Tap **Run Once**, **Allow 10 Runs**, or **Deny**. Approvals auto-expire after 5 minutes if unaddressed.

### 2. 🔒 Cryptographic Hash-Bound Execution
Approved scripts are copied to a content-addressed store and re-hashed right before process spawning. If a prompt-injection or race condition modifies even a single byte of the file between approval and execution, MAAS rejects the run and demands fresh authorization.

### 3. 🧠 100% Local AI Integration: Ollama & Gemma 4 Ready
MAAS includes an autonomous agent client ([`ollama_agent.py`](ollama_agent.py)) that connects locally running **Gemma 4** (via Ollama on Apple Silicon or Linux/Windows) to the remote MAAS compute node. Gemma autonomously reasons about the compute environment, drafts scripts, executes them remotely, and reports the synthesis back to you.

### 4. 🧯 Kernel Guardrails & Resilient Watchdogs
- **Memory Ceiling**: Real-time memory watchdog kills runaway memory hogs (`OOM_KILLED`) without taking down the node or the operating system.
- **Wall-Clock Timeouts**: Enforced hard deadlines terminate infinite loops.
- **Disk Floor Protection**: Rejects file creations and uploads if free disk space drops below 500 MB.
- **Process Tree Cleanup**: Terminates all spawned subprocesses and child trees cleanly on shutdown or cancellation.

### 5. 📊 Bigger-Than-RAM Dataset Streaming Engine
The built-in `ingest_dataset` tool streams gigabyte-scale CSV/TSV files into SQLite databases using constant memory (compatible with older laptops with low RAM). It automatically detects delimiters, UTF-8/BOM/UTF-16 encodings, repairs ragged rows, profiles column types and null counts, and exposes a read-only, row-capped `query_sql` interface.

### 6. 🔌 Multi-Client Protocol Adapters
- **Streamable HTTP MCP** (`/mcp`) for modern remote clients.
- **Stdio Bridge** ([`bridge.py`](bridge.py)) allowing local IDEs like **Cursor**, **Claude Desktop**, and **Antigravity** to communicate seamlessly over stdin/stdout.
- **Interactive Terminal CLI** ([`maas_cli.py`](maas_cli.py)) to test and operate nodes directly from your command line.

---

## 🛠️ The 16 MCP Tools

MAAS implements the full Model Context Protocol specification with 16 native tools:

| Tool | Category | Tier | Description |
|---|---|---|---|
| `get_node_capabilities` | System | 0 | Probes CPU model, cores, AVX/AVX2 features, available RAM, disk space, and discovered runtimes. |
| `list_files` | Workspace | 1 | Lists files and directories in the sandboxed workspace. |
| `read_file` | Workspace | 1 | Reads files safely from the node's workspace with path-traversal jailing. |
| `write_file` | Workspace | 1 | Writes or updates code/data files in the workspace. |
| `delete_file` | Workspace | 1 | Deletes files from the workspace. |
| `execute_script` | Execution | 2 | **Runs Python/Node/Bash/R scripts.** Bound by memory limit, timeout, and phone approval. |
| `get_task` | Tasks | 0 | Polls task state, exit code, resource utilization, and structured failure kinds. |
| `list_processes` | Tasks | 0 | Lists active and recent processes running on the node. |
| `read_process_logs` | Logs | 0 | Streams paged stdout/stderr logs (up to 8 KB per chunk) to avoid flooding conversation contexts. |
| `terminate_process` | Tasks | 0 | Kills a running or queued process tree instantly. Never requires approval. |
| `install_packages` | Packages | 2 | Installs Python (`pip`) or Node (`npm`) packages into MAAS's private prefix (not root). |
| `ingest_dataset` | Data | 2 | Streams tabular data (CSV/TSV) into SQLite with automated column profiling and row repair. |
| `describe_dataset` | Data | 0 | Returns dataset profile: row counts, column types, null counts, min/max values. |
| `list_datasets` | Data | 0 | Lists all ingested SQLite datasets and their file sizes. |
| `query_sql` | Data | 0 | Executes read-only SQL queries (`SELECT`) with strict row limits against ingested datasets. |
| `patch_file` | Workspace | 1 | Applies unified diffs to workspace files. |

---

## 🚀 Quick Start Guide

### Step 1: Install & Start on the Compute Node (e.g. Spare Linux/macOS Laptop)

```bash
git clone https://github.com/devmayank729/MAAS.git maas
cd maas
sh install.sh
maas start
```

*The terminal will display the node specs, the local network URL, an ASCII QR code, and a client pairing code.*

```text
  MAAS node "maas-node" is running  v0.1.0
  Ubuntu 24.04.4 LTS, AMD Ryzen 3 5300U (8 threads, AVX2), 15305 MB RAM
  Address  http://10.80.18.195:7420

  1. Scan with your phone to approve jobs (phone on the same Wi-Fi):
  [ QR Code ]
  http://10.80.18.195:7420/pair#k=wkqkmjthvc9x82x3

  2. Connect your AI client:
     curl -s -X POST http://10.80.18.195:7420/api/pair -H 'Content-Type: application/json' -d '{"code":"0585-3128"}'
```

---

### Step 2: Connect Phone Safety Switch

Scan the QR code displayed on the node's screen with your phone camera (or open the `/pair#k=...` link). You will see the **"Paired"** screen. Keep this tab open.

---

### Step 3: Connect Your AI Client (Pick Your Favorite)

#### Option A: Local Ollama Agent (Gemma 4 on Mac/PC)
If you run Ollama locally with `gemma4:12b` (or Llama 3 / Mistral):
```bash
python3 ollama_agent.py
```
*Gemma will load all 16 remote tools and chat with you in your terminal, offloading heavy execution to the node.*

#### Option B: Standalone Terminal CLI
Run tasks without any LLM directly from your main laptop:
```bash
# Check remote node specs:
python3 maas_cli.py specs

# Run a Python one-liner on the remote node:
python3 maas_cli.py run "import platform; print('Running on:', platform.node())"
```

#### Option C: Claude Code CLI
1. Exchange the one-time code for a client token:
   ```bash
   curl -s -X POST http://<NODE_IP>:7420/api/pair \
     -H 'Content-Type: application/json' \
     -d '{"code":"<8_DIGIT_CODE>"}'
   ```
2. Add the MCP server to Claude Code:
   ```bash
   claude mcp add --transport http --scope user maas-node http://<NODE_IP>:7420/mcp \
     --header "Authorization: Bearer <TOKEN>"
   ```

#### Option D: Cursor / Claude Desktop / Antigravity
Add the stdio bridge to your `mcp_config.json`:
```json
{
  "mcpServers": {
    "maas-node": {
      "command": "/usr/bin/python3",
      "args": [
        "/path/to/maas/bridge.py",
        "http://<NODE_IP>:7420/mcp",
        "<YOUR_TOKEN>"
      ]
    }
  }
}
```

---

## 🔬 Structured Failure Taxonomy (Self-Healing AI)

When an executed script encounters an error, MAAS returns structured diagnostic information rather than opaque exit codes so the AI agent can diagnose and fix its own code:

- `MODULE_NOT_FOUND`: Includes the missing module name and suggests a `pip` package candidate.
- `SYNTAX_ERROR`: Identifies the exact script file, line number, and offending character.
- `OOM_KILLED`: Flags that the job exceeded its memory quota, allowing the agent to suggest batching or streaming.
- `TIMEOUT`: Notifies the agent that execution exceeded the allocated wall-clock duration.
- `ILLEGAL_INSTRUCTION`: Warns that compiled binaries or wheels used CPU instructions (like AVX2) unsupported by older hardware.

---

## 📁 Repository Structure

```text
├── src/
│   ├── main.cpp              # CLI entry point, signal handling & runtime discovery
│   ├── server.cpp            # HTTP server, routing & MCP JSON-RPC handler
│   ├── tools.cpp             # Implementation of all 16 MCP tools
│   ├── tasks.cpp             # Process sandbox, limits, execution wrappers & watchdog
│   ├── ingest.cpp            # Chunked CSV/TSV parser, type inference & SQLite streaming
│   ├── webpage.cpp           # Embedded HTML/CSS phone approval & control interface
│   ├── auth.cpp              # One-time codes, SHA-256 token hashing & role enforcement
│   ├── platform_posix.cpp    # POSIX process isolation, memory monitoring & terminal IO
│   ├── platform_win.cpp      # Windows compatibility layer
│   ├── runtimes.cpp          # Automated discovery of Python, Node, Bash, Rscript
│   ├── qr.cpp                # Terminal UTF-8 and ANSI QR code generator
│   └── db.cpp                # SQLite state persistence & WAL journal management
├── third_party/              # Vendored dependencies (cpp-httplib, nlohmann/json, sqlite3)
├── tests/
│   └── e2e.py                # 30-check automated integration test suite
├── ollama_agent.py           # Fully local Ollama (Gemma 4) autonomous agent
├── maas_cli.py               # Lightweight remote client CLI
├── bridge.py                 # Stdio-to-HTTP MCP protocol bridge
├── build.sh                  # Native C++ build script (optimised / debug)
└── install.sh                # Zero-dependency OS installer
```

---

## 📜 License

MAAS is open-source software licensed under the **[MIT License](LICENSE)**.
Vendored libraries (`sqlite3`, `nlohmann/json`, `cpp-httplib`, `qrcodegen`) retain their respective open-source licenses.
