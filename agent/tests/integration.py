"""Runs the agent against a mock HTTP server and checks payload shape, auth header,
outage tolerance, and delivery of samples buffered during the outage.

usage: integration.py /path/to/meridian-agent
"""

import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

received: list[tuple[dict, dict]] = []
fail_mode = threading.Event()


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if fail_mode.is_set():
            self.send_response(503)
            self.end_headers()
            return
        received.append((dict(self.headers), json.loads(body)))
        self.send_response(202)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"accepted": 1}')

    def log_message(self, *a):
        pass


def start_server(port=0):
    srv = HTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def wait_for(pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.2)
    return False


def main(agent: str) -> int:
    srv = start_server()
    port = srv.server_address[1]
    env = {**os.environ, "API_URL": f"http://127.0.0.1:{port}/ingest", "API_KEY": "secret-key",
           "HOST_NAME": "itest", "INTERVAL_SECONDS": "1", "BATCH_SECONDS": "2"}
    proc = subprocess.Popen([agent], env=env, stderr=subprocess.PIPE, text=True)
    try:
        assert wait_for(lambda: len(received) >= 1, 15), "no batch received"
        headers, payload = received[0]
        assert headers["X-API-Key"] == "secret-key"
        assert headers["Content-Type"] == "application/json"
        assert payload["host"] == "itest" and payload["agent_version"]
        assert payload["samples"], "empty batch"
        for s in payload["samples"]:
            assert set(s) == {"ts", "metric", "device", "value"}, s
            assert re.fullmatch(r"[a-z0-9_.]+", s["metric"]), s
            assert isinstance(s["value"], (int, float)) and abs(time.time() - s["ts"]) < 60
        metrics = {s["metric"] for s in payload["samples"]}
        assert "mem.used_pct" in metrics

        # outage: server rejects -> agent must stay alive and keep the samples
        fail_mode.set()
        time.sleep(5)
        assert proc.poll() is None, "agent died during outage"
        before = len(received)
        fail_mode.clear()
        assert wait_for(lambda: len(received) > before, 40), "no recovery after outage"
        outage_ts = [s["ts"] for _, p in received[before:] for s in p["samples"]]
        assert min(outage_ts) < time.time() - 4, "buffered samples were not redelivered"
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            print("agent did not exit on SIGTERM")
            return 1
        srv.shutdown()
    print("agent integration test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
