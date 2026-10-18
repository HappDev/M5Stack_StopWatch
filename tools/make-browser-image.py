"""Make a first-install image for HTTPS browser flashers, starting at address 0.

Never use this image for vault-preserving updates: padding includes the NVS area.
Run with a Python environment containing esptool 4.12.0 (e.g. ESP-IDF 5.5.4).
"""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
from flash import verify, verify_browser_image

ROOT = Path(__file__).resolve().parents[1]

def main():
    directory = ROOT / 'dist' / 'firmware'
    manifest = verify(directory)
    image = directory / 'stopwatch-otp.factory.bin'
    subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32s3', 'merge_bin',
                    '--output', str(image), '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '16MB',
                    '0x0', str(directory / 'bootloader/bootloader.bin'),
                    '0x8000', str(directory / 'partition_table/partition-table.bin'),
                    '0x10000', str(directory / 'stopwatch_otp.bin')], check=True)
    merged = image.read_bytes()
    for offset, name in ((0x8000, 'partition_table/partition-table.bin'), (0x10000, 'stopwatch_otp.bin')):
        source = (directory / name).read_bytes()
        if merged[offset:offset + len(source)] != source:
            raise SystemExit('Merged image mismatch: ' + name)
    if len(merged) != 0x10000 + (directory / 'stopwatch_otp.bin').stat().st_size:
        raise SystemExit('Unexpected merged image length')
    metadata = {'offset': 0, 'firstInstallOnly': True, 'inputs': manifest['files'],
                'size': len(merged), 'sha256': hashlib.sha256(merged).hexdigest()}
    (directory / 'browser-image.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    verify_browser_image(directory)
    print('Verified browser image (FIRST INSTALL ONLY): ' + str(image))

if __name__ == '__main__':
    main()
