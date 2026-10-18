"""Static site and release download. Loopback by default; LAN is opt-in."""
import argparse
import socket
import ssl
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import threading
import webbrowser
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1] / 'web'
RELEASE = ROOT.parent / 'dist' / 'StopWatch-OTP-0.1.0.zip'
BROWSER_IMAGE = ROOT.parent / 'dist' / 'firmware' / 'stopwatch-otp.factory.bin'

class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, '.mjs': 'text/javascript; charset=utf-8'}

    def _serve(self, head=False):
        allowed = getattr(self.server, 'allowed_hosts', {'127.0.0.1', 'localhost'})
        if self.headers.get('Host') not in {f'{host}:{self.server.server_port}' for host in allowed}:
            self.send_error(403)
            return
        downloads = {'/download/StopWatch-OTP-0.1.0.zip': RELEASE,
                     '/download/stopwatch-otp.factory.bin': BROWSER_IMAGE}
        download = downloads.get(urlsplit(self.path).path)
        if download:
            try:
                with download.open('rb') as release:
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/zip' if download.suffix == '.zip' else 'application/octet-stream')
                    self.send_header('Content-Length', str(download.stat().st_size))
                    self.send_header('Content-Disposition', f'attachment; filename="{download.name}"')
                    self.end_headers()
                    if not head:
                        self.copyfile(release, self.wfile)
            except FileNotFoundError:
                self.send_error(404, 'Release package not found')
            return
        if head:
            super().do_HEAD()
        else:
            super().do_GET()

    def do_GET(self):
        self._serve()

    def do_HEAD(self):
        self._serve(head=True)

    def list_directory(self, path):
        self.send_error(404)
        return None

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'no-referrer')
        self.send_header('Permissions-Policy', 'serial=(self), camera=(self), microphone=(), geolocation=()')
        self.send_header('Content-Security-Policy', "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' blob: data:; media-src 'self' blob:; font-src 'self'; connect-src 'none'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'")
        super().end_headers()

    def log_message(self, fmt, *args):
        # Do not retain requested URLs or anything that could have been pasted into one.
        pass

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--host', default='127.0.0.1', help='Use 0.0.0.0 to listen on the local network')
    parser.add_argument('--no-browser', action='store_true')
    parser.add_argument('--cert', type=Path, help='HTTPS certificate PEM, including intermediate chain if needed')
    parser.add_argument('--key', type=Path, help='HTTPS private key PEM (kept outside the web directory)')
    parser.add_argument('--allow-host', action='append', default=[], help='Additional hostname present in the certificate')
    args = parser.parse_args()
    if bool(args.cert) != bool(args.key):
        parser.error('--cert and --key must be provided together')
    scheme = 'https' if args.cert else 'http'
    context = None
    if args.cert:
        for secret_path in (args.cert, args.key):
            if secret_path.resolve().is_relative_to(ROOT.resolve()):
                parser.error('TLS files must be outside the public web directory')
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.cert, args.key)
    with ThreadingHTTPServer((args.host, args.port), partial(Handler, directory=str(ROOT))) as server:
        if context:
            server.socket = context.wrap_socket(server.socket, server_side=True)
        server.allowed_hosts = {'127.0.0.1', 'localhost'}
        server.allowed_hosts.update(args.allow_host)
        if args.host == '0.0.0.0':
            server.allowed_hosts.update(socket.gethostbyname_ex(socket.gethostname())[2])
        else:
            server.allowed_hosts.add(args.host)
        url = f'{scheme}://127.0.0.1:{server.server_port}/'
        print(f'StopWatch OTP: {url}', flush=True)
        for address in sorted(server.allowed_hosts - {'localhost', '127.0.0.1'}):
            print(f'LAN: {scheme}://{address}:{server.server_port}/', flush=True)
        print('Open this address in Chrome or Edge. Ctrl+C stops the local server.', flush=True)
        if not args.no_browser:
            threading.Timer(0.4, webbrowser.open, args=(url,)).start()
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass

if __name__ == '__main__':
    main()
