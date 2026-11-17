"""Serve the UI simulator and reload it on every change: serve.py [port]. Run by `rake ui:dev`.
Rebuilds ui/build/ui.wasm when a C file under ui/ changes; the page reloads itself through /events."""
import http.server
import pathlib
import subprocess
import sys
import threading
import time

UI = pathlib.Path(__file__).resolve().parent.parent
ROOT = UI.parent
SIM = UI / "sim"
WASM = UI / "build" / "ui.wasm"
version = 0
changed = threading.Condition()


def watched():
    for path in list(UI.glob("src/*")) + list(UI.glob("fonts/*.[ch]")) + list(SIM.glob("*")):
        if path.is_file() and path.suffix in {".c", ".h", ".html", ".js", ".css"}:
            yield path


def watch():
    global version
    seen = {p: p.stat().st_mtime for p in watched()}
    while True:
        time.sleep(0.25)
        now = {p: p.stat().st_mtime for p in watched()}
        edited = [p for p in now if seen.get(p) != now[p]]
        seen = now
        if not edited:
            continue
        if any(p.suffix in {".c", ".h"} for p in edited):
            t0 = time.time()
            result = subprocess.run(["rake", "ui:wasm"], cwd=ROOT, capture_output=True, text=True)
            if result.returncode != 0:
                print(result.stdout + result.stderr, flush=True)
                continue
            print(f"rebuilt in {time.time() - t0:.1f} s", flush=True)
        with changed:
            version += 1
            changed.notify_all()


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(SIM), **kwargs)

    def log_message(self, *args):
        pass

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        if self.path == "/ui.wasm":
            data = WASM.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/wasm")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        elif self.path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            with changed:
                start = version
                changed.wait_for(lambda: version != start)
            try:
                self.wfile.write(b"data: reload\n\n")
            except OSError:
                pass
        else:
            super().do_GET()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8790
    threading.Thread(target=watch, daemon=True).start()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True
    print(f"UI simulator at http://127.0.0.1:{port}/", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
