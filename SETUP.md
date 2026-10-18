# Setting up MAAS

You need two machines on the same network:

- **The node** — the old laptop that will run the code.
- **Your main laptop** — where your AI client (Claude Code) runs.

Plus your phone, for approvals. Event and campus Wi-Fi often blocks device-to-device traffic; if anything below cannot connect, put both laptops and the phone on **your phone's hotspot** — that fixes it almost every time.

## 1. Install on the node (Linux or macOS)

```sh
git clone <your-repo-url> maas
cd maas
sh install.sh
```

`install.sh` detects Linux or macOS, installs a C++ compiler if one is missing (apt, dnf, yum, pacman, zypper or apk; on macOS it opens the Command Line Tools installer), builds MAAS, copies it to `~/.maas/bin/maas` and adds that folder to your PATH. Open a new terminal afterwards.

Scripts need a runtime. If the node has no Python with pip:

```sh
maas runtime                  # shows what was found
maas runtime install python   # portable Python into ~/.maas/runtimes, no admin rights
```

**Windows:** not supported by the installer yet. The source has Windows support but it is untested; use a Linux or macOS machine as the node for now.

## 2. Start the node

```sh
maas start
```

It prints the node's specs, its address (for example `http://192.168.1.23:7420`), a QR code, and a one-time code for your AI client. If port 7420 is busy it picks the next free one and tells you. Leave this terminal open: you can also approve jobs here with `y` / `n` / `a`.

Useful options: `maas start --port 8000`, `maas start --ephemeral` (deletes everything on exit).

From another terminal on the node: `maas status`, `maas stop`, and `maas pair` (fresh codes and QR; codes work once and expire).

## 3. Pair your phone

Scan the QR code with the phone's camera and open the link. The page says "Paired". From now on, every script, package install or deletion the AI asks for appears there with the exact code, a Deny button and a Run button. You can also stop jobs and pause the node from that page.

If the page will not load: the phone is not on the same network, or the Wi-Fi blocks devices from talking to each other. Use the phone hotspot.

## 4. Connect Claude Code (on your main laptop)

`maas connect` (which will do this automatically) is not built yet, so it is two commands. Use the address and the 8-digit code the node printed.

**Get a token** (the code works once):

```sh
curl -s -X POST http://192.168.1.23:7420/api/pair \
  -H 'Content-Type: application/json' -d '{"code":"1234-5678","label":"my laptop"}'
```

Windows PowerShell on the main laptop:

```powershell
Invoke-RestMethod -Method Post -Uri http://192.168.1.23:7420/api/pair -ContentType 'application/json' -Body '{"code":"1234-5678","label":"my laptop"}'
```

The reply contains `"token": "maas_..."`.

**Add the node to Claude Code:**

```sh
claude mcp add --transport http --scope user maas-node http://192.168.1.23:7420/mcp \
  --header "Authorization: Bearer maas_..."
claude mcp list        # should say: maas-node ... ✓ Connected
```

Then ask Claude something like: *"Use the maas-node tools. Check what the node can do, then write and run a Python script that prints the first 10 primes."* Approve it on your phone.

If the node's IP address changes (new Wi-Fi), remove and re-add the server with the new address: `claude mcp remove maas-node --scope user`.

**Other clients:** any MCP client that supports remote HTTP servers with a custom `Authorization` header can use the same URL and token (LM Studio's `mcp.json` takes `"url"` and `"headers"`; not tested). **Claude Desktop cannot connect yet**: it needs the `maas bridge` command, which is not implemented.

## 5. Send a big data file

Open the node's address in your main laptop's browser after pairing that browser with the QR link (or use the phone), and use "Send a data file". Uploads resume if the connection drops. The file lands in `uploads/` on the node. Then ask the AI to `ingest_dataset` it and explore it with `query_sql`.

## 6. Check everything works

On any machine with Python 3 that can reach the node, get fresh codes with `maas pair` on the node and run:

```sh
python3 tests/e2e.py http://<node-ip>:7420 <client-code> <phone-code>
```

It runs 30 checks (pairing, auth, MCP, approvals, failures, memory kill, timeout, uploads, datasets) and ends with `ALL PASSED`. It writes small test files into the node's workspace and adds a dataset called `small`.

## Configuration

`~/.maas/config/maas.json` is created on first start. Common settings:

| Key | Default | Meaning |
|---|---|---|
| `name` | `maas-node` | Node name (also `<name>.local`) |
| `port` | `7420` | HTTP port |
| `approval_timeout_sec` | `300` | Undecided approvals are denied after this |
| `default_memory_mb` | 60% of RAM | Memory limit per job |
| `default_timeout_sec` | `900` | Time limit per job |
| `max_concurrent_jobs` | 1 (2+ on 4+ cores) | Jobs at once |
| `disk_floor_mb` | `500` | No new jobs or uploads below this free space |
| `package_allowlist` | `[]` | If set, only these packages may be installed |
| `allow_public` | `false` | Answer non-private addresses (leave off) |

A typo or wrong type stops the node with the line and column of the problem; delete the file to get defaults back.

## Troubleshooting

| Problem | Fix |
|---|---|
| `curl` or the phone cannot reach the node | Same network? Try the phone hotspot. On Linux with a firewall: `sudo ufw allow 7420/tcp`. |
| `claude mcp list` shows the node as failed | Check the URL ends in `/mcp` and the header is `Authorization: Bearer maas_...`. A token from a used code is fine; a revoked or mistyped one gives 401. |
| "that pairing code is wrong or was already used" | Run `maas pair` on the node for fresh codes. |
| Jobs fail with `RUNTIME_NOT_FOUND` | `maas runtime install python` on the node, then restart it. |
| `ILLEGAL_INSTRUCTION` | The old CPU lacks AVX/AVX2; ask the AI to use packages that do not need them. |
| HTTPS / pip errors on the node | Check the node's date and time; old laptops lose the clock when the CMOS battery is dead. |

## Removing MAAS

Stop the node (`maas stop`) and delete `~/.maas`. Remove the `# MAAS` line from `~/.bashrc`, `~/.zshrc` and `~/.profile`.
