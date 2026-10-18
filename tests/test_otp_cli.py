import contextlib
import getpass
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('otp_cli', Path(__file__).resolve().parents[1] / 'tools/otp_cli.py')
cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cli)
SECRET = 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ'  # Public RFC test vector.

class FakeSerial:
    def __init__(self, **kwargs):
        self.is_open = False
        self.incoming = bytearray()
        self.sent = []
        self.drop = None
        self.reply = None
        self.dtr = None
        self.rts = None

    def open(self):
        assert self.dtr is False and self.rts is False
        self.is_open = True

    def reset_input_buffer(self):
        self.incoming.clear()

    def write(self, data):
        request = json.loads(data)
        self.sent.append(request)
        if self.drop == request['op']:
            return len(data)
        response = self.reply(request) if self.reply else {'v':1, 'id':request['id'], 'ok':True, 'result':{'state':'unlocked', 'authorized':True}}
        if isinstance(response, bytes):
            self.incoming.extend(response)
        else:
            self.incoming.extend((json.dumps(response, ensure_ascii=False) + '\n').encode())
        return len(data)

    def read(self, size):
        result = bytes(self.incoming[:size])
        del self.incoming[:size]
        return result

    def close(self):
        self.is_open = False

class CliTests(unittest.TestCase):
    def test_base32_uri_and_limits(self):
        row = cli.parse_uri('otpauth://totp/Test:alice?secret=' + SECRET + '&issuer=Test&algorithm=SHA512&digits=8&period=60')
        self.assertEqual((row['issuer'], row['account'], row['algorithm'], row['digits'], row['period']), ('Test','alice','SHA512',8,60))
        self.assertEqual(row['secret'], SECRET)
        self.assertEqual(cli.normalize_record({'issuer':'Example','secret':SECRET.lower()})['secret'], SECRET)
        for uri in ('otpauth://hotp/a?secret=' + SECRET,
                    'otpauth://totp/a?secret=' + SECRET + '&secret=' + SECRET,
                    'otpauth://totp/One:a?issuer=Two&secret=' + SECRET,
                    'otpauth://totp/a?secret=' + SECRET + '&counter=1',
                    'otpauth://totp/%ZZ?secret=' + SECRET):
            with self.subTest(uri=uri), self.assertRaises(cli.CliError):
                cli.parse_uri(uri)
        for overrides in ({'secret': SECRET + 'MZ'}, {'secret':'A'}, {'digits':7}, {'digits':6.0},
                          {'period':True}, {'issuer':'я'*49}, {'account':'a\x00'}, {'algorithm':'MD5'}):
            with self.subTest(overrides=overrides), self.assertRaises(cli.CliError):
                cli.normalize_record({'issuer':'Test','secret':SECRET, **overrides})

    def test_import_validates_entire_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'accounts.json'
            row = {'id':'12345678-1234-4234-8234-123456789abc','issuer':'Test','secret':SECRET}
            path.write_text(json.dumps([row]))
            self.assertEqual(len(cli.read_records(path)), 1)
            for invalid in ([row,row], [row,{'issuer':'bad','secret':'bad'}], {'format':'stopwatch-otp'}, [row]*101):
                path.write_text(json.dumps(invalid))
                with self.assertRaises(cli.CliError):
                    cli.read_records(path)
            path.write_text('{"records":[],"records":[]}')
            with self.assertRaises(cli.CliError):
                cli.read_records(path)

    def test_dtr_handshake_and_fragmented_unicode(self):
        fake = FakeSerial()
        with cli.Device('test', serial_factory=lambda **kwargs: fake) as device:
            self.assertTrue(fake.dtr)
            fake.reply = lambda req: {'v':1,'id':req['id'],'ok':True,'result':{'text':'Тест'}}
            self.assertEqual(device.request('status')['text'], 'Тест')
        self.assertFalse(fake.dtr)
        self.assertFalse(fake.is_open)

    def test_read_only_handshake_can_retry(self):
        fake = FakeSerial()
        original = cli.Device.request
        attempts = []
        def request(device, op, *args, **kwargs):
            attempts.append(op)
            if len(attempts) == 1:
                raise cli.RequestTimeout('dropped during DTR transition')
            return original(device, op, *args, **kwargs)
        with patch.object(cli.Device, 'request', request):
            with cli.Device('test', serial_factory=lambda **kwargs: fake):
                pass
        self.assertEqual(attempts, ['status','status'])

    def test_timeout_never_retries_write(self):
        fake = FakeSerial()
        with cli.Device('test', serial_factory=lambda **kwargs: fake) as device:
            fake.drop = 'upsert'
            with self.assertRaises(cli.RequestTimeout):
                device.request('upsert', {'record':{'secret':SECRET}}, timeout=0.005)
        self.assertEqual([r['op'] for r in fake.sent], ['status','upsert'])

    def test_bad_frames_and_errors_do_not_echo_secret(self):
        fake = FakeSerial()
        with cli.Device('test', serial_factory=lambda **kwargs: fake) as device:
            for response in (b'x' * 4097, b'\0\n', b'{broken}\n',
                             b'{"id":2,"id":2}\n'):
                fake.incoming.clear()
                fake.reply = lambda req, result=response: result
                with self.assertRaises(cli.CliError):
                    device.request('status', timeout=0.01)
            fake.incoming.clear()
            fake.reply = lambda req: {'v':1,'id':req['id'],'ok':False,'error':{'code':'CANCELLED','message':SECRET}}
            with self.assertRaises(cli.DeviceError) as caught:
                device.request('upsert', {'record':{'secret':SECRET}})
            self.assertEqual(caught.exception.code, 'CANCELLED')
            self.assertNotIn(SECRET, str(caught.exception))

    def test_authorization_wait_does_not_reopen_port(self):
        fake = FakeSerial()
        with cli.Device('test', serial_factory=lambda **kwargs: fake) as device:
            states = iter([{'state':'locked','authorized':False}, {'state':'unlocked','authorized':True}])
            fake.reply = lambda req: {'v':1,'id':req['id'],'ok':True,'result':next(states)}
            with patch.object(cli.time, 'sleep'), contextlib.redirect_stdout(io.StringIO()):
                self.assertTrue(device.wait_authorized()['authorized'])
            self.assertTrue(fake.dtr)

    def test_hidden_input_refuses_echo_fallback(self):
        with patch.object(cli.getpass, 'getpass', side_effect=getpass.GetPassWarning('echo')):
            with self.assertRaises(cli.CliError):
                cli.hidden_input('secret: ')

if __name__ == '__main__':
    unittest.main()
