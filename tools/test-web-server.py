"""Serve the real UI with a conspicuous test-only USB fixture on loopback:8766."""
from functools import partial
from http.server import ThreadingHTTPServer
from pathlib import Path
from server import Handler, ROOT

class FixtureHandler(Handler):
    def do_GET(self):
        if self.path == '/':
            text = (ROOT / 'index.html').read_text(encoding='utf-8')
            text = text.replace('<script type="module" src="app.mjs">', '<script type="module" src="fixture.mjs"></script><script type="module" src="app.mjs">')
            data = text.encode()
        elif self.path == '/fixture.mjs':
            data = (Path(__file__).resolve().parents[1] / 'tests/fixtures/serial.js').read_bytes()
        else:
            return super().do_GET()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8' if self.path == '/' else 'text/javascript')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

if __name__ == '__main__':
    with ThreadingHTTPServer(('127.0.0.1',8766),partial(FixtureHandler,directory=str(ROOT))) as server:
        print('TEST FIXTURE: http://127.0.0.1:8766/ (no physical USB)',flush=True)
        try:server.serve_forever()
        except KeyboardInterrupt:pass
