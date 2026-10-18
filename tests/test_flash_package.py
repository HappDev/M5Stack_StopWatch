import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('otp_flash', Path(__file__).resolve().parents[1] / 'tools/flash.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class FlashTests(unittest.TestCase):
    def create(self, root):
        directory = root / 'dist/firmware'
        files = {}
        for name in module.IMAGES:
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            data = b'public-test-binary'
            path.write_bytes(data)
            files[name] = {'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)}
        manifest = {'chip':'esp32s3','idf':'5.5.4','version':'test','secureBoot':False,'flashEncryption':False,'efuseProgramming':False,'files':files}
        (directory / 'manifest.json').write_text(json.dumps(manifest))
        return directory

    def test_integrity_and_security_guard(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = self.create(root)
            module.verify(directory)
            (directory / 'stopwatch_otp.bin').write_bytes(b'tampered')
            with self.assertRaises(SystemExit):
                module.verify(directory)
            directory = self.create(root)
            manifest = json.loads((directory / 'manifest.json').read_text())
            manifest['efuseProgramming'] = True
            (directory / 'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(SystemExit):
                module.verify(directory)

    def test_update_never_writes_partition_table_or_vault(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.create(root)
            with patch.object(module, 'ROOT', root), patch.object(sys, 'argv', ['flash.py','--port','COM7','--update']), patch.object(module.subprocess, 'run') as run:
                module.main()
                command = run.call_args.args[0]
                self.assertIn('0x10000', command)
                self.assertNotIn('0x0', command)
                self.assertNotIn('0x8000', command)
                self.assertNotIn('0x710000', command)
                self.assertNotIn('0x790000', command)
                self.assertFalse(any('efuse' in value.lower() for value in command))

    def test_browser_image_rejects_tampering_and_stale_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = self.create(Path(temporary))
            manifest = module.verify(directory)
            merged = bytearray(b'\xff' * (0x10000 + manifest['files']['stopwatch_otp.bin']['size']))
            for name, (offset, _) in module.IMAGES.items():
                source = (directory / name).read_bytes()
                merged[offset:offset + len(source)] = source
            image = directory / 'stopwatch-otp.factory.bin'
            image.write_bytes(merged)
            metadata = {'offset':0, 'firstInstallOnly':True, 'inputs':manifest['files'],
                        'sha256':hashlib.sha256(merged).hexdigest(), 'size':len(merged)}
            info = directory / 'browser-image.json'
            info.write_text(json.dumps(metadata))
            module.verify_browser_image(directory)
            merged[10] ^= 1
            image.write_bytes(merged)
            with self.assertRaises(SystemExit):
                module.verify_browser_image(directory)
            merged[10] ^= 1
            image.write_bytes(merged)
            metadata['inputs'] = {}
            info.write_text(json.dumps(metadata))
            with self.assertRaises(SystemExit):
                module.verify_browser_image(directory)

if __name__ == '__main__':
    unittest.main()
