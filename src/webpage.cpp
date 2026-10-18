// The pairing and status page served at http://<node>:7420/ . It is the phone
// "safety switch": approve or deny the exact script, see jobs, stop everything.
// Not a dashboard: one screen, a few KB, embedded in the binary, works offline.
#include "webpage.hpp"

namespace maas {

const std::string& page_html() {
  static const std::string html = std::string(R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#123a2c">
<title>MAAS node</title>
<style>
:root{
  --board:#123a2c; --panel:#184838; --panel-2:#1f5644; --line:#2d6a55;
  --silk:#eef2ea; --muted:#a9c2b4; --brass:#d9ae52; --stop:#e2554b; --ok:#8fd19e;
  --code-bg:#0e2e23;
  --mono:ui-monospace,"SF Mono","Cascadia Mono","Roboto Mono",Menlo,Consolas,monospace;
}
*{box-sizing:border-box}
html,body{margin:0;background:var(--board);color:var(--silk);
  font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans",sans-serif;-webkit-text-size-adjust:100%}
body{padding:16px max(16px,env(safe-area-inset-right)) 48px max(16px,env(safe-area-inset-left))}
main{max-width:640px;margin:0 auto}
h1{font-size:1.25rem;margin:0;font-weight:700;letter-spacing:.01em}
h2{font-size:1.05rem;margin:28px 0 10px;font-weight:650}
p{margin:6px 0}
.muted{color:var(--muted)}
.small{font-size:.875rem}
button{font:inherit;cursor:pointer;border-radius:10px;padding:12px 16px;border:2px solid var(--silk);
  background:transparent;color:var(--silk);min-height:48px}
button:focus-visible,a:focus-visible,input:focus-visible{outline:3px solid var(--brass);outline-offset:2px}
button[disabled]{opacity:.5;cursor:default}
a{color:var(--brass)}
.top{display:flex;justify-content:space-between;align-items:flex-start;gap:12px}
.dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:var(--ok);margin-right:6px;vertical-align:1px}
.dot.off{background:var(--stop)}
.spec{margin-top:4px}
.controls{display:flex;gap:10px;margin-top:14px;flex-wrap:wrap}
.controls button{flex:1 1 140px}
.stop{border-color:var(--stop);color:#fff;background:var(--stop)}
.paused-banner{margin-top:14px;padding:10px 12px;border-left:4px solid var(--brass);background:var(--panel)}

/* The approval: the one thing on this page that must be impossible to miss. */
.approval{margin-top:20px;background:var(--panel);border:2px solid var(--brass);border-radius:16px;padding:16px;
  box-shadow:0 0 0 6px rgba(217,174,82,.12)}
.approval h3{margin:0 0 2px;font-size:1.2rem;word-break:break-all}
.facts{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin:12px 0;font-size:.9rem}
.facts dt{color:var(--muted)}
.facts dd{margin:0;word-break:break-all}
.hash{font-family:var(--mono);color:var(--brass)}
.ai{margin:10px 0;padding:10px 12px;border-left:3px solid var(--muted);color:var(--muted);font-size:.92rem}
.ai strong{color:var(--silk);font-weight:600}
pre{margin:10px 0 0;background:var(--code-bg);border-radius:10px;padding:12px;overflow:auto;max-height:46vh;
  font:13px/1.5 var(--mono);white-space:pre;tab-size:4}
pre .add{color:var(--ok)} pre .del{color:#f19a92}
.tabs{display:flex;gap:6px;margin-top:12px}
.tabs button{min-height:36px;padding:6px 12px;border-width:1px;font-size:.875rem}
.tabs button[aria-pressed=true]{background:var(--silk);color:var(--board)}
.decide{display:grid;grid-template-columns:1fr 1.4fr;gap:10px;margin-top:16px}
.deny{border-color:var(--stop);color:#ffd7d3}
.run{background:var(--silk);color:var(--board);border-color:var(--silk);font-weight:700}
.many{margin-top:10px;background:none;border:none;color:var(--brass);padding:6px 0;min-height:0;text-decoration:underline}
.expires{font-size:.85rem;color:var(--muted);margin-top:8px}

.jobs{list-style:none;margin:0;padding:0}
.jobs li{border-top:1px solid var(--line);padding:12px 0}
.job-head{display:flex;justify-content:space-between;align-items:center;gap:10px}
.job-name{font-weight:600;word-break:break-all}
.state{font-size:.8rem;padding:2px 8px;border-radius:999px;border:1px solid var(--muted);color:var(--muted);white-space:nowrap}
.state.RUNNING{border-color:var(--brass);color:var(--brass)}
.state.ok{border-color:var(--ok);color:var(--ok)}
.state.bad{border-color:var(--stop);color:#ffb3ac}
.job-actions{display:flex;gap:8px;margin-top:8px}
.job-actions button{min-height:36px;padding:6px 12px;font-size:.875rem;border-width:1px}
.empty{color:var(--muted);padding:8px 0}
.box{background:var(--panel);border-radius:12px;padding:14px}
.code-big{font:600 2rem/1.2 var(--mono);letter-spacing:.06em;color:var(--brass);margin:6px 0}
.cmd{font:13px/1.4 var(--mono);background:var(--code-bg);padding:10px;border-radius:8px;word-break:break-all;margin-top:8px}
.drop{border:2px dashed var(--line);border-radius:12px;padding:18px;text-align:center}
.drop.over{border-color:var(--brass)}
progress{width:100%;height:10px;margin-top:10px;accent-color:var(--brass)}
.toast{position:fixed;left:16px;right:16px;bottom:16px;max-width:600px;margin:0 auto;background:var(--silk);color:var(--board);
  padding:12px 14px;border-radius:10px;font-weight:600;display:none}
.center{min-height:70vh;display:flex;flex-direction:column;justify-content:center}
@media (prefers-reduced-motion:no-preference){.approval{animation:arrive .35s ease-out}
  @keyframes arrive{from{transform:translateY(8px);opacity:0}to{transform:none;opacity:1}}}
</style>
</head>
<body>
<main id="app"><div class="center"><h1>MAAS node</h1><p class="muted">Loading…</p></div></main>
<div class="toast" id="toast" role="status" aria-live="polite"></div>
<script src="/app.js"></script>
</body>
</html>
)HTML");
  return html;
}

const std::string& page_js() {
  static const std::string js = std::string(R"JS("use strict";
(function () {
  var TOKEN_KEY = "maas_approver_token";
  var app = document.getElementById("app");
  var toastBox = document.getElementById("toast");
  var token = null;
  var lastData = null;
  var openLogs = {};
  var viewMode = {};
  var pollTimer = null;
  var busy = false;

  function load() { try { return localStorage.getItem(TOKEN_KEY); } catch (e) { return null; } }
  function save(value) { try { localStorage.setItem(TOKEN_KEY, value); } catch (e) {} }
  function forget() { try { localStorage.removeItem(TOKEN_KEY); } catch (e) {} }

  function el(tag, attrs, children) {
    var node = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (key) {
        if (key === "text") node.textContent = attrs[key];
        else if (key === "cls") node.className = attrs[key];
        else if (key.indexOf("on") === 0) node.addEventListener(key.slice(2), attrs[key]);
        else node.setAttribute(key, attrs[key]);
      });
    }
    (children || []).forEach(function (child) {
      if (child === null || child === undefined) return;
      node.appendChild(typeof child === "string" ? document.createTextNode(child) : child);
    });
    return node;
  }

  function toast(message) {
    toastBox.textContent = message;
    toastBox.style.display = "block";
    clearTimeout(toast.timer);
    toast.timer = setTimeout(function () { toastBox.style.display = "none"; }, 3200);
  }

  function api(method, path, body) {
    var options = { method: method, headers: { "Authorization": "Bearer " + token } };
    if (body !== undefined) {
      options.headers["Content-Type"] = "application/json";
      options.body = JSON.stringify(body);
    }
    return fetch(path, options).then(function (response) {
      if (response.status === 401) { forget(); token = null; showUnpaired("This phone is no longer paired."); throw new Error("unpaired"); }
      return response.json().then(function (data) {
        if (!response.ok) throw new Error(data.error || ("HTTP " + response.status));
        return data;
      });
    });
  }

  function size(bytes) {
    if (bytes === undefined || bytes === null) return "";
    var units = ["B", "KB", "MB", "GB", "TB"]; var value = bytes; var unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; unit++; }
    return (unit === 0 ? value : value.toFixed(1)) + " " + units[unit];
  }

  function duration(seconds) {
    if (seconds === undefined || seconds === null) return "";
    if (seconds < 60) return seconds + " s";
    if (seconds < 3600) return Math.floor(seconds / 60) + " min " + (seconds % 60) + " s";
    return (seconds / 3600).toFixed(1) + " h";
  }

  function clock(seconds) {
    var minutes = Math.floor(seconds / 60); var rest = seconds % 60;
    return minutes + ":" + (rest < 10 ? "0" : "") + rest;
  }

  // ------------------------------------------------------------ pairing
  function pairFromHash() {
    var match = /[#&]k=([a-z0-9-]+)/i.exec(location.hash || "");
    if (!match) return false;
    var code = match[1];
    history.replaceState(null, "", location.pathname);
    app.textContent = "";
    app.appendChild(el("div", { cls: "center" }, [el("h1", { text: "Pairing this phone…" })]));
    fetch("/api/pair", { method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ code: code, label: "phone (" + (navigator.platform || "browser") + ")" }) })
      .then(function (response) { return response.json().then(function (data) { return { ok: response.ok, data: data }; }); })
      .then(function (result) {
        if (!result.ok || !result.data.token) { showUnpaired(result.data.error || "Pairing failed."); return; }
        token = result.data.token; save(token);
        toast("Paired. Approvals will appear here.");
        start();
      })
      .catch(function () { showUnpaired("Could not reach the node. Is this phone on the same Wi-Fi?"); });
    return true;
  }

  function showUnpaired(reason) {
    stopPolling();
    app.textContent = "";
    app.appendChild(el("div", { cls: "center" }, [
      el("h1", { text: "Scan the QR code on the node" }),
      el("p", { cls: "muted", text: reason || "This page needs to be opened from the QR code that the node prints when it starts." }),
      el("p", { cls: "muted small", text: "No QR on screen? Run  maas pair  on the node to show a fresh one." })
    ]));
  }

  // ------------------------------------------------------------ rendering
  // A key of everything that matters on screen; polling only redraws when it
  // changes, so reading a long script is never interrupted.
  function screenKey(data) {
    return JSON.stringify({
      paused: data.paused,
      pending: (data.pending || []).map(function (p) { return [p.id, p.state]; }),
      tasks: (data.tasks || []).map(function (t) { return [t.task_id, t.state, t.failure_kind, t.log_bytes, t.progress && t.progress.percent, t.duration_sec > 120 ? Math.floor(t.duration_sec / 60) : t.duration_sec]; })
    });
  }
  var lastKey = "";

  function render(data) {
    lastData = data;
    lastKey = screenKey(data);
    var node = data.node || {};
    var scrolls = {};
    Array.prototype.forEach.call(document.querySelectorAll("pre[data-key]"), function (pre) { scrolls[pre.getAttribute("data-key")] = [pre.scrollTop, pre.scrollLeft]; });
    app.textContent = "";
    var head = el("div", { cls: "top" }, [
      el("div", null, [
        el("h1", null, [el("span", { cls: "dot", "aria-hidden": "true" }), node.name || "MAAS node"]),
        el("p", { cls: "muted small spec", text: [node.os, node.cores + " cores", Math.round((node.ram_free_mb || 0)) + " of " + Math.round(node.ram_total_mb || 0) + " MB free", size((node.disk_free_mb || 0) * 1048576) + " disk free"].join(", ") })
      ])
    ]);
    app.appendChild(head);

    if (data.paused) app.appendChild(el("div", { cls: "paused-banner", text: "Paused. Approved jobs wait here until you resume." }));

    var pending = data.pending || [];
    if (pending.length) {
      app.appendChild(renderApproval(pending[0], pending.length));
    } else {
      app.appendChild(el("p", { cls: "muted", style: "margin-top:18px", text: "Nothing waiting for approval. When the AI wants to run something, it appears here." }));
    }

    var controls = el("div", { cls: "controls" }, [
      el("button", { text: data.paused ? "Resume jobs" : "Pause jobs", onclick: function () {
        api("POST", "/api/pause", { paused: !data.paused }).then(refresh).catch(function (e) { toast(e.message); });
      } }),
      el("button", { cls: "stop", text: "Stop everything", onclick: function () {
        if (!confirm("Stop every running job and cancel everything waiting?")) return;
        api("POST", "/api/stop-all", {}).then(function (r) { toast("Stopped " + r.stopped + " tasks."); refresh(); }).catch(function (e) { toast(e.message); });
      } })
    ]);
    app.appendChild(controls);

    app.appendChild(el("h2", { text: "Jobs" }));
    var tasks = data.tasks || [];
    if (!tasks.length) app.appendChild(el("p", { cls: "empty", text: "No jobs yet." }));
    var list = el("ul", { cls: "jobs" });
    tasks.forEach(function (task) { list.appendChild(renderTask(task)); });
    app.appendChild(list);

    app.appendChild(el("h2", { text: "Connect an AI client" }));
    app.appendChild(renderPairClient());

    app.appendChild(el("h2", { text: "Send a data file to the node" }));
    app.appendChild(renderUpload());
    Array.prototype.forEach.call(document.querySelectorAll("pre[data-key]"), function (pre) {
      var saved = scrolls[pre.getAttribute("data-key")];
      if (saved) { pre.scrollTop = saved[0]; pre.scrollLeft = saved[1]; }
    });
  }

  function updateCountdown(data) {
    var pending = data.pending || [];
    var line = document.getElementById("expires");
    if (line && pending.length) line.textContent = "Denied automatically in " + clock(pending[0].expires_in_sec || 0) + " if you do nothing.";
  }
)JS") + std::string(R"JS(
  function renderApproval(item, count) {
    var box = el("section", { cls: "approval", "aria-label": "Approval needed" });
    var kindLabel = { script: "Run a script", install: "Install packages", "delete": "Delete a file", ingest: "Replace a dataset" }[item.kind] || item.kind;
    box.appendChild(el("p", { cls: "muted small", text: kindLabel + (count > 1 ? " (1 of " + count + " waiting)" : "") }));
    box.appendChild(el("h3", { text: item.kind === "install" ? (item.packages || []).join(", ") : (item.name || "") }));
    var facts = el("dl", { cls: "facts" });
    function fact(label, value, cls) {
      if (value === undefined || value === null || value === "") return;
      facts.appendChild(el("dt", { text: label }));
      facts.appendChild(el("dd", { cls: cls || "", text: String(value) }));
    }
    if (item.kind === "script") {
      fact("Runs with", item.interpreter);
      fact("Arguments", (item.args || []).length ? item.args.join(" ") : "none");
      fact("Size", size(item.size) + ", " + item.lines + " lines");
      fact("SHA-256", (item.sha256 || "").slice(0, 16), "hash");
      if (item.previous_failures) fact("Failed before", item.previous_failures + " time(s)");
    }
    if (item.kind === "install") fact("Installer", item.manager);
    if (item.kind === "delete") { fact("Path", item.path); fact("Size", size(item.size)); }
    fact("Limits", item.memory_mb + " MB memory, " + duration(item.timeout_sec) + " time");
    fact("Asked by", item.requested_by);
    box.appendChild(facts);
    if (item.ai_says) {
      box.appendChild(el("div", { cls: "ai" }, [el("strong", { text: "AI says: " }), item.ai_says]));
    }
    if (item.kind === "script") {
      var mode = viewMode[item.id] || (item.diff ? "diff" : "code");
      var tabs = el("div", { cls: "tabs", role: "group", "aria-label": "Show" });
      if (item.diff) {
        tabs.appendChild(el("button", { text: "Changes", "aria-pressed": String(mode === "diff"), onclick: function () { viewMode[item.id] = "diff"; render(lastData); } }));
      }
      tabs.appendChild(el("button", { text: "Full script", "aria-pressed": String(mode === "code"), onclick: function () { viewMode[item.id] = "code"; render(lastData); } }));
      box.appendChild(tabs);
      var pre = el("pre", { tabindex: "0", "data-key": "approval-" + item.id + "-" + mode });
      if (mode === "diff" && item.diff) {
        item.diff.split("\n").forEach(function (line) {
          var span = el("span", { cls: line.charAt(0) === "+" ? "add" : (line.charAt(0) === "-" ? "del" : ""), text: line + "\n" });
          pre.appendChild(span);
        });
      } else {
        pre.textContent = item.content || "";
      }
      box.appendChild(pre);
    }
    var decide = el("div", { cls: "decide" }, [
      el("button", { cls: "deny", text: "Deny", onclick: function () { decideOn(item.id, "deny"); } }),
      el("button", { cls: "run", text: item.kind === "script" ? "Run once" : "Approve", onclick: function () { decideOn(item.id, "approve"); } })
    ]);
    box.appendChild(decide);
    if (item.kind === "script") {
      box.appendChild(el("button", { cls: "many", text: "Run and allow this exact script 10 more times", onclick: function () { decideOn(item.id, "approve_many"); } }));
    }
    box.appendChild(el("p", { cls: "expires", id: "expires", text: "Denied automatically in " + clock(item.expires_in_sec || 0) + " if you do nothing." }));
    return box;
  }

  function decideOn(id, decision) {
    if (busy) return; busy = true;
    api("POST", "/api/approvals/" + encodeURIComponent(id), { decision: decision })
      .then(function (result) { toast(result.message || "Done."); })
      .catch(function (e) { toast(e.message); })
      .then(function () { busy = false; refresh(); });
  }

  function stateClass(task) {
    if (task.state === "EXITED") return task.failure_kind === "OK" ? "ok" : "bad";
    if (task.state === "KILLED" || task.state === "LOST" || task.state === "DENIED" || task.state === "EXPIRED") return "bad";
    return task.state;
  }

  function stateText(task) {
    if (task.state === "EXITED") return task.failure_kind === "OK" ? "done" : task.failure_kind.toLowerCase().replace(/_/g, " ");
    return task.state.toLowerCase();
  }

  function renderTask(task) {
    var item = el("li");
    var meta = [];
    if (task.duration_sec !== undefined) meta.push(duration(task.duration_sec));
    if (task.peak_memory_mb) meta.push(task.peak_memory_mb + " MB peak");
    if (task.progress && task.progress.percent !== undefined) meta.push(task.progress.percent + "%");
    item.appendChild(el("div", { cls: "job-head" }, [
      el("div", null, [el("div", { cls: "job-name", text: task.name || task.task_id }), el("div", { cls: "muted small", text: meta.join(", ") })]),
      el("span", { cls: "state " + stateClass(task), text: stateText(task) })
    ]));
    if (task.result && task.result.summary && task.failure_kind !== "OK") item.appendChild(el("p", { cls: "muted small", text: task.result.summary }));
    var actions = el("div", { cls: "job-actions" });
    if (task.state === "RUNNING" || task.state === "APPROVED" || task.state === "PENDING") {
      actions.appendChild(el("button", { text: task.state === "RUNNING" ? "Stop" : "Cancel", onclick: function () {
        api("POST", "/api/tasks/" + encodeURIComponent(task.task_id) + "/kill", {}).then(refresh).catch(function (e) { toast(e.message); });
      } }));
    }
    if (task.log_bytes) {
      actions.appendChild(el("button", { text: openLogs[task.task_id] ? "Hide output" : "Show output", onclick: function () {
        if (openLogs[task.task_id]) { delete openLogs[task.task_id]; render(lastData); return; }
        openLogs[task.task_id] = "…"; render(lastData); fetchLog(task.task_id);
      } }));
    }
    item.appendChild(actions);
    if (openLogs[task.task_id]) item.appendChild(el("pre", { text: openLogs[task.task_id], "data-key": "log-" + task.task_id }));
    return item;
  }

  function fetchLog(id) {
    api("GET", "/api/tasks/" + encodeURIComponent(id) + "/log").then(function (result) {
      if (openLogs[id] !== undefined && openLogs[id] !== result.log) { openLogs[id] = result.log || "(no output yet)"; render(lastData); }
    }).catch(function () {});
  }

  var clientCode = null;
  function renderPairClient() {
    var box = el("div", { cls: "box" });
    if (clientCode) {
      box.appendChild(el("p", { cls: "muted small", text: "On your main laptop, run:" }));
      box.appendChild(el("div", { cls: "cmd", text: "maas connect " + location.origin + " --code " + clientCode.code }));
      box.appendChild(el("p", { cls: "code-big", text: clientCode.code }));
      box.appendChild(el("p", { cls: "muted small", text: "Works once, for " + Math.round(clientCode.expires_in / 60) + " minutes." }));
    } else {
      box.appendChild(el("p", { cls: "muted small", text: "Claude Code, Claude Desktop and LM Studio connect with a one-time code." }));
      box.appendChild(el("button", { text: "Show a connection code", onclick: function () {
        api("POST", "/api/pair-code", { role: "client" }).then(function (result) { clientCode = result; render(lastData); }).catch(function (e) { toast(e.message); });
      } }));
    }
    return box;
  }

  var upload = { file: null, sent: 0, running: false, message: "" };
  function renderUpload() {
    var box = el("div", { cls: "drop" });
    var input = el("input", { type: "file", id: "file", style: "display:none", onchange: function (event) { startUpload(event.target.files[0]); } });
    box.appendChild(input);
    if (upload.running || upload.message) {
      box.appendChild(el("p", { text: upload.file ? upload.file.name : "" }));
      var bar = el("progress", { max: "100", value: String(upload.file ? Math.floor(100 * upload.sent / Math.max(1, upload.file.size)) : 0) });
      box.appendChild(bar);
      box.appendChild(el("p", { cls: "muted small", text: upload.message || (size(upload.sent) + " of " + size(upload.file.size)) }));
    } else {
      box.appendChild(el("p", { cls: "muted small", text: "Drop a CSV/TSV here or pick one. It goes to uploads/ on the node, ready for the AI to ingest. Large files resume if the connection drops." }));
      box.appendChild(el("button", { text: "Choose a file", onclick: function () { input.click(); } }));
    }
    box.addEventListener("dragover", function (event) { event.preventDefault(); box.classList.add("over"); });
    box.addEventListener("dragleave", function () { box.classList.remove("over"); });
    box.addEventListener("drop", function (event) { event.preventDefault(); box.classList.remove("over"); if (event.dataTransfer.files[0]) startUpload(event.dataTransfer.files[0]); });
    return box;
  }

  function cleanName(name) { return name.replace(/[^A-Za-z0-9._-]+/g, "_").replace(/^[._]+/, "").slice(0, 120) || "upload.dat"; }

  function startUpload(file) {
    if (!file || upload.running) return;
    upload = { file: file, sent: 0, running: true, message: "" };
    var name = cleanName(file.name);
    var chunk = 8 * 1024 * 1024;
    var base = "/api/upload?name=" + encodeURIComponent(name);
    function finish(message) { upload.running = false; upload.message = message; render(lastData); }
    function send(offset, attempt) {
      if (offset >= file.size) {
        api("POST", "/api/upload/complete?name=" + encodeURIComponent(name) + "&total=" + file.size + "&overwrite=1", {})
          .then(function (r) { finish("Uploaded as " + r.path + ". Ask the AI to ingest it."); })
          .catch(function (e) { finish("Upload failed: " + e.message); });
        return;
      }
      var piece = file.slice(offset, Math.min(file.size, offset + chunk));
      fetch(base + "&offset=" + offset + "&total=" + file.size, { method: "PUT", headers: { "Authorization": "Bearer " + token, "Content-Type": "application/octet-stream" }, body: piece })
        .then(function (response) { return response.json().then(function (data) { return { status: response.status, data: data }; }); })
        .then(function (result) {
          if (result.status === 409 && result.data.size !== undefined) { send(result.data.size, 0); return; }
          if (result.status >= 400) throw new Error(result.data.error || ("HTTP " + result.status));
          upload.sent = result.data.size; render(lastData); send(result.data.size, 0);
        })
        .catch(function (e) {
          if (attempt < 5) { setTimeout(function () { send(offset, attempt + 1); }, 1500 * (attempt + 1)); return; }
          finish("Upload stopped: " + e.message + ". Pick the file again to resume.");
        });
    }
    fetch(base, { headers: { "Authorization": "Bearer " + token } })
      .then(function (response) { return response.json(); })
      .then(function (info) { var start = info.partial_size && info.partial_size <= file.size ? info.partial_size : 0; upload.sent = start; render(lastData); send(start, 0); })
      .catch(function () { send(0, 0); });
  }

  // ------------------------------------------------------------ polling
  function refresh() {
    if (!token) return;
    api("GET", "/api/status").then(function (data) {
      if (busy) return;
      if (screenKey(data) !== lastKey || !lastData) render(data);
      else { lastData = data; updateCountdown(data); }
    }).catch(function (error) {
      if (error.message === "unpaired") return;
      var dot = document.querySelector(".dot"); if (dot) dot.classList.add("off");
    });
  }

  function stopPolling() { if (pollTimer) { clearInterval(pollTimer); pollTimer = null; } }

  function start() {
    refresh();
    stopPolling();
    pollTimer = setInterval(function () {
      if (document.visibilityState === "visible") {
        refresh();
        Object.keys(openLogs).forEach(fetchLog);
      }
    }, 2000);
  }

  document.addEventListener("visibilitychange", function () { if (document.visibilityState === "visible" && token) refresh(); });

  if (!pairFromHash()) {
    token = load();
    if (token) start(); else showUnpaired();
  }
})();
)JS");
  return js;
}

}  // namespace maas
