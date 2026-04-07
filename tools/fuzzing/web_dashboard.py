#!/usr/bin/env python3
"""Web dashboard for Donner continuous fuzzing.

Serves a live HTML dashboard showing fuzzing status, coverage trends,
corpus growth, and crash history. Zero external dependencies — uses
only Python stdlib.

Usage:
    python3 tools/fuzzing/web_dashboard.py                # Port 8080
    python3 tools/fuzzing/web_dashboard.py --port=9090    # Custom port
"""

import argparse
import html
import json
import os
import sys
import time
from datetime import datetime, timezone
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dashboard import (
    format_duration,
    load_corpus_history,
    load_known_crashes,
    load_run_reports,
)
from run_continuous_fuzz import STATE_DIR


RUNS_DIR = STATE_DIR / "runs"
TRIGGER_LOGS_DIR = STATE_DIR / "trigger-logs"


# ---------------------------------------------------------------------------
# Data helpers
# ---------------------------------------------------------------------------

def get_trigger_status() -> dict:
    """Get status of the trigger (last run time, next expected, lock)."""
    status = {
        "last_run_time": None,
        "last_run_commit": None,
        "lock_pid": None,
        "is_running": False,
    }

    ts_file = STATE_DIR / "last_run_timestamp"
    if ts_file.exists():
        try:
            ts = int(ts_file.read_text().strip())
            status["last_run_time"] = datetime.fromtimestamp(ts, tz=timezone.utc).strftime(
                "%Y-%m-%d %H:%M:%S UTC"
            )
        except (ValueError, OSError):
            pass

    commit_file = STATE_DIR / "last_run_commit"
    if commit_file.exists():
        status["last_run_commit"] = commit_file.read_text().strip()[:12]

    lock_file = STATE_DIR / "trigger.lock"
    if lock_file.exists():
        try:
            pid = int(lock_file.read_text().strip())
            status["lock_pid"] = pid
            # Check if process is actually running
            try:
                os.kill(pid, 0)
                status["is_running"] = True
            except OSError:
                pass
        except (ValueError, OSError):
            pass

    return status


def get_latest_trigger_log(max_lines: int = 50) -> str:
    """Get the tail of the latest trigger log."""
    if not TRIGGER_LOGS_DIR.is_dir():
        return "(no trigger logs found)"
    logs = sorted(TRIGGER_LOGS_DIR.iterdir(), reverse=True)
    for log_file in logs:
        if log_file.is_file() and log_file.suffix == ".log":
            lines = log_file.read_text().splitlines()
            tail = lines[-max_lines:]
            return "\n".join(tail)
    return "(no trigger logs found)"


# ---------------------------------------------------------------------------
# HTML rendering
# ---------------------------------------------------------------------------

def render_status_badge(is_running: bool) -> str:
    if is_running:
        return '<span class="badge running">FUZZING</span>'
    return '<span class="badge idle">IDLE</span>'


def render_health(reports: list[dict], crashes: dict, trigger: dict) -> str:
    if not reports:
        return '<div class="card"><h2>No runs yet</h2><p>Waiting for first fuzzing run...</p></div>'

    latest = reports[0]
    total_execs = sum(f.get("total_execs", 0) for f in latest.get("fuzzers", []))
    total_fuzzers = latest.get("total_fuzzers", 0)
    plateau_count = sum(
        1 for f in latest.get("fuzzers", [])
        if "plateau" in f.get("exit_reason", "")
    )
    still_growing = [
        f["name"] for f in latest.get("fuzzers", [])
        if f.get("exit_reason") not in ("plateau", "completed", "no_binary", "error", "crash")
        and f.get("exit_reason") is not None
    ]

    return f"""
    <div class="card">
      <h2>Status {render_status_badge(trigger['is_running'])}</h2>
      <div class="metrics">
        <div class="metric">
          <div class="metric-value">{total_fuzzers}</div>
          <div class="metric-label">Fuzzers</div>
        </div>
        <div class="metric">
          <div class="metric-value">{total_execs:,}</div>
          <div class="metric-label">Executions (last run)</div>
        </div>
        <div class="metric">
          <div class="metric-value">{len(crashes)}</div>
          <div class="metric-label">Known Crashes</div>
        </div>
        <div class="metric">
          <div class="metric-value">{plateau_count}/{total_fuzzers}</div>
          <div class="metric-label">Plateaued</div>
        </div>
      </div>
      <p class="detail">Last run: {latest.get('_name', '?')} &middot;
         Duration: {format_duration(latest.get('total_duration_secs', 0))} &middot;
         Commit: <code>{trigger.get('last_run_commit', '?')}</code></p>
      {"<p class='detail'>Still growing: " + ", ".join(f"<code>{n}</code>" for n in still_growing[:5]) + "</p>" if still_growing else ""}
    </div>"""


def render_runs_table(reports: list[dict]) -> str:
    if not reports:
        return ""

    rows = ""
    for r in reports:
        name = r.get("_name", "?")
        duration = format_duration(r.get("total_duration_secs", 0))
        fuzzers = r.get("total_fuzzers", 0)
        execs = sum(f.get("total_execs", 0) for f in r.get("fuzzers", []))
        crashes = r.get("total_crashes", 0)
        plateau = sum(1 for f in r.get("fuzzers", []) if "plateau" in f.get("exit_reason", ""))
        deadline = sum(1 for f in r.get("fuzzers", []) if f.get("exit_reason") == "deadline")

        crash_class = ' class="crash"' if crashes > 0 else ""
        rows += f"""
        <tr>
          <td><code>{html.escape(name)}</code></td>
          <td>{duration}</td>
          <td>{fuzzers}</td>
          <td>{execs:,}</td>
          <td{crash_class}>{crashes}</td>
          <td>{plateau}</td>
          <td>{deadline}</td>
        </tr>"""

    return f"""
    <div class="card">
      <h2>Recent Runs</h2>
      <table>
        <thead>
          <tr>
            <th>Run</th><th>Duration</th><th>Fuzzers</th><th>Execs</th>
            <th>Crashes</th><th>Plateau</th><th>Deadline</th>
          </tr>
        </thead>
        <tbody>{rows}</tbody>
      </table>
    </div>"""


def render_coverage_trends(reports: list[dict]) -> str:
    if len(reports) < 1:
        return ""

    all_fuzzers = set()
    for r in reports:
        for f in r.get("fuzzers", []):
            all_fuzzers.add(f["name"])

    run_names = [r["_name"][-8:] for r in reversed(reports)]
    header_cols = "".join(f"<th>{html.escape(n)}</th>" for n in run_names)

    rows = ""
    for fuzzer_name in sorted(all_fuzzers):
        cells = ""
        prev_cov = 0
        for r in reversed(reports):
            cov = 0
            for f in r.get("fuzzers", []):
                if f["name"] == fuzzer_name:
                    cov = f.get("peak_coverage", 0)
                    break
            if cov > 0:
                trend = ""
                if prev_cov > 0 and cov > prev_cov:
                    trend = f' <span class="trend-up">+{cov - prev_cov}</span>'
                cells += f"<td>{cov:,}{trend}</td>"
            else:
                cells += "<td class='muted'>&mdash;</td>"
            if cov > 0:
                prev_cov = cov

        rows += f"<tr><td><code>{html.escape(fuzzer_name)}</code></td>{cells}</tr>"

    return f"""
    <div class="card">
      <h2>Coverage Trends</h2>
      <table>
        <thead><tr><th>Fuzzer</th>{header_cols}</tr></thead>
        <tbody>{rows}</tbody>
      </table>
    </div>"""


def render_corpus_history(history: list[dict]) -> str:
    if not history:
        return ""

    rows = ""
    for entry in history[-10:]:
        ts = entry.get("timestamp", "?")
        if "T" in ts:
            ts = ts[:19].replace("T", " ")
        total = entry.get("total_after", 0)
        rows += f"<tr><td>{html.escape(ts)}</td><td>{total:,}</td></tr>"

    return f"""
    <div class="card">
      <h2>Corpus History</h2>
      <table>
        <thead><tr><th>Timestamp</th><th>Total Minimized Inputs</th></tr></thead>
        <tbody>{rows}</tbody>
      </table>
    </div>"""


def render_crashes(crashes: dict) -> str:
    if not crashes:
        return """
        <div class="card">
          <h2>Crashes</h2>
          <p class="ok">No crashes found. Parsers are holding up well.</p>
        </div>"""

    rows = ""
    for sig, info in sorted(crashes.items(), key=lambda x: x[1].get("date", ""), reverse=True):
        date = html.escape(info.get("date", "?")[:10])
        fuzzer = html.escape(info.get("fuzzer", "?"))
        crash_type = html.escape(info.get("crash_type", "?"))
        top_frame = html.escape(info.get("top_frame", "?"))
        issue_url = info.get("issue_url")
        issue_link = f'<a href="{html.escape(issue_url)}">{html.escape(issue_url)}</a>' if issue_url else "N/A"

        rows += f"""
        <tr>
          <td><code>{html.escape(sig)}</code></td>
          <td><code>{fuzzer}</code></td>
          <td>{crash_type}</td>
          <td><code>{top_frame}</code></td>
          <td>{date}</td>
          <td>{issue_link}</td>
        </tr>"""

    return f"""
    <div class="card">
      <h2>Crashes ({len(crashes)})</h2>
      <table>
        <thead>
          <tr><th>Signature</th><th>Fuzzer</th><th>Type</th><th>Top Frame</th><th>Date</th><th>Issue</th></tr>
        </thead>
        <tbody>{rows}</tbody>
      </table>
    </div>"""


def render_log(log_text: str) -> str:
    return f"""
    <div class="card">
      <h2>Latest Trigger Log</h2>
      <pre>{html.escape(log_text)}</pre>
    </div>"""


def render_page(max_runs: int = 10) -> str:
    reports = load_run_reports(max_runs)
    history = load_corpus_history()
    crashes = load_known_crashes()
    trigger = get_trigger_status()
    log_text = get_latest_trigger_log()
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="30">
<title>Donner Fuzzing Dashboard</title>
<style>
  :root {{
    --bg: #0d1117;
    --card-bg: #161b22;
    --border: #30363d;
    --text: #c9d1d9;
    --text-muted: #8b949e;
    --accent: #58a6ff;
    --green: #3fb950;
    --red: #f85149;
    --orange: #d29922;
  }}
  * {{ box-sizing: border-box; margin: 0; padding: 0; }}
  body {{
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif;
    background: var(--bg);
    color: var(--text);
    line-height: 1.5;
    padding: 20px;
    max-width: 1400px;
    margin: 0 auto;
  }}
  h1 {{
    font-size: 24px;
    margin-bottom: 4px;
  }}
  .header {{
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    margin-bottom: 20px;
    border-bottom: 1px solid var(--border);
    padding-bottom: 12px;
  }}
  .header .updated {{
    color: var(--text-muted);
    font-size: 13px;
  }}
  .card {{
    background: var(--card-bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 16px 20px;
    margin-bottom: 16px;
  }}
  .card h2 {{
    font-size: 16px;
    margin-bottom: 12px;
    color: var(--accent);
  }}
  .metrics {{
    display: flex;
    gap: 32px;
    margin-bottom: 12px;
  }}
  .metric {{
    text-align: center;
  }}
  .metric-value {{
    font-size: 32px;
    font-weight: 600;
  }}
  .metric-label {{
    font-size: 12px;
    color: var(--text-muted);
    text-transform: uppercase;
    letter-spacing: 0.5px;
  }}
  .detail {{
    color: var(--text-muted);
    font-size: 13px;
    margin-top: 4px;
  }}
  .badge {{
    display: inline-block;
    padding: 2px 8px;
    border-radius: 12px;
    font-size: 12px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    vertical-align: middle;
    margin-left: 8px;
  }}
  .badge.running {{
    background: rgba(63, 185, 80, 0.2);
    color: var(--green);
    animation: pulse 2s ease-in-out infinite;
  }}
  .badge.idle {{
    background: rgba(139, 148, 158, 0.2);
    color: var(--text-muted);
  }}
  @keyframes pulse {{
    0%, 100% {{ opacity: 1; }}
    50% {{ opacity: 0.5; }}
  }}
  table {{
    width: 100%;
    border-collapse: collapse;
    font-size: 13px;
  }}
  th, td {{
    padding: 6px 10px;
    text-align: left;
    border-bottom: 1px solid var(--border);
  }}
  th {{
    color: var(--text-muted);
    font-weight: 600;
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.5px;
  }}
  td {{
    font-variant-numeric: tabular-nums;
  }}
  td.crash {{
    color: var(--red);
    font-weight: 600;
  }}
  td.muted {{
    color: var(--text-muted);
  }}
  .trend-up {{
    color: var(--green);
    font-size: 11px;
  }}
  .ok {{
    color: var(--green);
  }}
  code {{
    font-family: 'SF Mono', 'Fira Code', monospace;
    font-size: 12px;
    background: rgba(110, 118, 129, 0.1);
    padding: 1px 5px;
    border-radius: 3px;
  }}
  pre {{
    font-family: 'SF Mono', 'Fira Code', monospace;
    font-size: 11px;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 12px;
    overflow-x: auto;
    max-height: 300px;
    overflow-y: auto;
    color: var(--text-muted);
    line-height: 1.4;
  }}
  a {{
    color: var(--accent);
    text-decoration: none;
  }}
  a:hover {{
    text-decoration: underline;
  }}
</style>
</head>
<body>
  <div class="header">
    <h1>Donner Fuzzing Dashboard</h1>
    <span class="updated">Auto-refreshes every 30s &middot; {now}</span>
  </div>
  {render_health(reports, crashes, trigger)}
  {render_runs_table(reports)}
  {render_coverage_trends(reports)}
  {render_corpus_history(history)}
  {render_crashes(crashes)}
  {render_log(log_text)}
</body>
</html>"""


# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------

class DashboardHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            content = render_page().encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)
        elif self.path == "/api/status":
            data = {
                "trigger": get_trigger_status(),
                "runs": load_run_reports(10),
                "corpus_history": load_corpus_history(),
                "known_crashes": load_known_crashes(),
            }
            # Strip internal fields
            for r in data["runs"]:
                r.pop("_dir", None)
            content = json.dumps(data, indent=2).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)
        else:
            self.send_error(404)

    def log_message(self, format, *args):
        # Quieter logging — just method + path
        sys.stderr.write(f"[dashboard] {args[0]}\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Fuzzing web dashboard")
    parser.add_argument("--port", type=int, default=8080, help="Port (default: 8080)")
    parser.add_argument("--bind", type=str, default="0.0.0.0", help="Bind address (default: 0.0.0.0)")
    args = parser.parse_args()

    server = HTTPServer((args.bind, args.port), DashboardHandler)
    print(f"Dashboard running at http://{args.bind}:{args.port}/")
    print(f"  HTML: http://{args.bind}:{args.port}/")
    print(f"  JSON: http://{args.bind}:{args.port}/api/status")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()


if __name__ == "__main__":
    main()
