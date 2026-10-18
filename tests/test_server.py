import importlib.util
from functools import partial
from http.client import HTTPConnection
from http.server import ThreadingHTTPServer
from pathlib import Path
import threading
import unittest

spec = importlib.util.spec_from_file_location('otp_server', Path(__file__).resolve().parents[1] / 'tools/server.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(('127.0.0.1', 0), partial(module.Handler, directory=str(module.ROOT)))
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def request(self, path='/', method='GET', headers=None):
        conn = HTTPConnection('127.0.0.1', self.server.server_port)
        conn.request(method, path, headers=headers or {})
        response = conn.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        conn.close()
        return result

    def test_headers_and_modules(self):
        status, headers, body = self.request()
        self.assertEqual(status, 200)
        self.assertIn('StopWatch'.encode(), body)
        self.assertEqual(headers['Cache-Control'], 'no-store')
        self.assertIn("connect-src 'none'", headers['Content-Security-Policy'])
        self.assertIn('serial=(self)', headers['Permissions-Policy'])
        self.assertIn('text/javascript', self.request('/otp.mjs')[1]['Content-type'])

    def test_no_uploads_or_source_escape(self):
        self.assertEqual(self.request('/', 'POST')[0], 501)
        self.assertEqual(self.request('/../LICENSE')[0], 404)
        self.assertEqual(self.request('/vendor/')[0], 404)
        self.assertEqual(self.request('/', headers={'Host': 'attacker.example'})[0], 403)
        self.assertEqual(self.request('/', 'HEAD', headers={'Host': 'attacker.example'})[0], 403)

    def test_release_and_lan_hosts(self):
        self.server.allowed_hosts = {'127.0.0.1', 'localhost', '192.168.1.50'}
        try:
            status, _, _ = self.request(headers={'Host': f'192.168.1.50:{self.server.server_port}'})
            self.assertEqual(status, 200)
            self.assertEqual(self.request('/', headers={'Host': f'attacker.example:{self.server.server_port}'})[0], 403)
            self.assertEqual(self.request('/dist/firmware/manifest.json')[0], 404)
            if module.RELEASE.is_file():
                status, headers, body = self.request('/download/StopWatch-OTP-0.1.0.zip')
                self.assertEqual(status, 200)
                self.assertEqual(headers['Content-Type'], 'application/zip')
                self.assertEqual(body, module.RELEASE.read_bytes())
            if module.BROWSER_IMAGE.is_file():
                status, headers, body = self.request('/download/stopwatch-otp.factory.bin')
                self.assertEqual(status, 200)
                self.assertEqual(headers['Content-Type'], 'application/octet-stream')
                self.assertEqual(body, module.BROWSER_IMAGE.read_bytes())
        finally:
            del self.server.allowed_hosts

if __name__ == '__main__':
    unittest.main()
