"""Flash the checked prebuilt package; no rebuild and no eFuse commands."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
IMAGES = {'bootloader/bootloader.bin': (0x0, 0x8000),
          'partition_table/partition-table.bin': (0x8000, 0x1000),
          'stopwatch_otp.bin': (0x10000, 0x700000)}

def verify(directory):
    directory = Path(directory)
    manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('chip') != 'esp32s3' or manifest.get('idf') != '5.5.4' or any(
            manifest.get(key) is not False for key in ('secureBoot', 'flashEncryption', 'efuseProgramming')):
        raise SystemExit('Unsupported firmware or security settings')
    if set(manifest.get('files', {})) != set(IMAGES):
        raise SystemExit('Unexpected firmware files')
    for name, (_, limit) in IMAGES.items():
        data = (directory / name).read_bytes()
        spec = manifest['files'][name]
        if not data or len(data) > limit or len(data) != spec['size'] or hashlib.sha256(data).hexdigest() != spec['sha256']:
            raise SystemExit('Firmware integrity check failed: ' + name)
    return manifest

def verify_browser_image(directory):
    directory = Path(directory)
    manifest = verify(directory)
    image = (directory / 'stopwatch-otp.factory.bin').read_bytes()
    metadata = json.loads((directory / 'browser-image.json').read_text(encoding='utf-8'))
    if (metadata.get('inputs') != manifest['files'] or metadata.get('offset') != 0
            or metadata.get('firstInstallOnly') is not True
            or metadata.get('sha256') != hashlib.sha256(image).hexdigest()
            or metadata.get('size') != len(image)):
        raise SystemExit('Browser image is damaged or stale; run tools/make-browser-image.py')
    for name in ('partition_table/partition-table.bin', 'stopwatch_otp.bin'):
        source = (directory / name).read_bytes()
        offset = IMAGES[name][0]
        if image[offset:offset + len(source)] != source:
            raise SystemExit('Browser image differs from checked firmware: ' + name)
    if len(image) != 0x10000 + manifest['files']['stopwatch_otp.bin']['size']:
        raise SystemExit('Unexpected browser image size')
    return metadata

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port')
    parser.add_argument('--update', action='store_true', help='Write only the application; preserve partition table and vault')
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    directory = ROOT / 'dist' / 'firmware'
    manifest = verify(directory)
    print('Verified StopWatch OTP ' + manifest['version'])
    if args.verify_only:
        return
    if not args.port:
        parser.error('--port COMx is required')
    cmd = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', args.port,
           '--baud', '460800', '--before', 'default_reset', '--after', 'hard_reset',
           'write_flash', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '16MB']
    for name, (offset, _) in IMAGES.items():
        if not args.update or name == 'stopwatch_otp.bin':
            cmd += [hex(offset), str(directory / name)]
    subprocess.run(cmd, check=True)

if __name__ == '__main__':
    main()
