"""Package source, the offline site and checked firmware; exclude build caches and test data."""
from pathlib import Path
import zipfile
from flash import verify, verify_browser_image

ROOT = Path(__file__).resolve().parents[1]

def main():
    verify(ROOT / 'dist/firmware')
    if (ROOT / 'dist/firmware/stopwatch-otp.factory.bin').exists():
        verify_browser_image(ROOT / 'dist/firmware')
    paths = [ROOT / name for name in ('README.md', 'LICENSE', 'THIRD_PARTY.md', 'dependencies.json', 'package.json', 'start-web.cmd', 'start-web-lan.cmd', 'start-web-lan-mac.command', 'otp-usb.cmd', '.gitignore', '.gitattributes')]
    for folder in ('web', 'docs', 'tools', 'tests', 'firmware/main', 'firmware/assets', 'dist/firmware'):
        paths += [p for p in (ROOT / folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc']
    paths += [ROOT / 'firmware' / name for name in ('CMakeLists.txt','sdkconfig.defaults','partitions.csv','dependencies.lock')]
    output = ROOT / 'dist' / 'StopWatch-OTP-0.1.0.zip'
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(set(paths)):
            archive.write(path, 'StopWatch-OTP/' + path.relative_to(ROOT).as_posix())
    print(output)

if __name__ == '__main__':
    main()
