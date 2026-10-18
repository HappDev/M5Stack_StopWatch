"""Build in a local cache: Windows cmd/ninja cannot build inside a UNC share."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
from bootstrap import main as bootstrap

ROOT = Path(__file__).resolve().parents[1]

def verify_configuration(config):
    text = Path(config).read_text(encoding='utf-8')
    forbidden = re.findall(r'^(CONFIG_(?:SECURE_BOOT|SECURE_BOOT_ENABLED|SECURE_BOOT_V2_ENABLED|SECURE_FLASH_ENC_ENABLED|NVS_ENCRYPTION|BOOTLOADER_APP_ANTI_ROLLBACK|ESP_COREDUMP_ENABLE_TO_FLASH|ESP_COREDUMP_ENABLE_TO_UART))=y$', text, re.MULTILINE)
    if forbidden:
        raise SystemExit('Refusing firmware with irreversible/security settings: ' + ', '.join(forbidden))
    # In a minimal build the entire core-dump component/Kconfig is absent.
    for required in ('CONFIG_ESP_CONSOLE_NONE=y', 'CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT=y', 'CONFIG_LOG_DEFAULT_LEVEL_NONE=y'):
        if required not in text:
            raise SystemExit('Refusing firmware with console/core dumps enabled: ' + required)

def copy_if_changed(source, target):
    source, target = Path(source), Path(target)
    if target.exists() and source.stat().st_size == target.stat().st_size and source.stat().st_mtime_ns == target.stat().st_mtime_ns:
        return str(target)
    return shutil.copy2(source, target)

def main():
    os.environ.setdefault('PYTHONUTF8', '1')
    os.environ.setdefault('PYTHONIOENCODING', 'utf-8')
    parser = argparse.ArgumentParser()
    parser.add_argument('--port')
    parser.add_argument('--flash', action='store_true')
    parser.add_argument('--update', action='store_true')
    parser.add_argument('--configure-only', action='store_true')
    args = parser.parse_args()
    idf = os.environ.get('IDF_PATH')
    if not idf:
        raise SystemExit('Open ESP-IDF 5.5.4 PowerShell first (IDF_PATH is missing).')
    if (args.flash or args.update) and not args.port:
        raise SystemExit('Specify --port COMx. Close the website USB connection first.')
    bootstrap()
    identity = hashlib.sha256(str(ROOT).encode()).hexdigest()[:12]
    # USERPROFILE avoids Windows Store virtualization expanding LOCALAPPDATA into
    # a long package path which exceeds GCC's dependency-file path limit.
    cache = Path(os.environ.get('STOPWATCH_BUILD_CACHE', Path.home() / '.sotp')) / identity
    stage = cache / 'firmware'
    shutil.copytree(ROOT / 'firmware', stage, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('build', '.git', 'managed_components', 'sdkconfig', 'sdkconfig.old'),
                    copy_function=copy_if_changed)
    print(f'Build source: {stage}', flush=True)
    command = [sys.executable, str(Path(idf) / 'tools' / 'idf.py'), '-C', str(stage)]
    subprocess.run(command + ['reconfigure' if args.configure_only else 'build'], check=True)
    verify_configuration(stage / 'sdkconfig')
    lock = stage / 'dependencies.lock'
    if lock.exists():
        shutil.copy2(lock, ROOT / 'firmware' / 'dependencies.lock')
    if not args.configure_only:
        output = ROOT / 'dist' / 'firmware'
        for name in ('stopwatch_otp.bin', 'bootloader/bootloader.bin', 'partition_table/partition-table.bin', 'flash_args', 'flasher_args.json'):
            target = output / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(stage / 'build' / name, target)
        binaries = ('stopwatch_otp.bin', 'bootloader/bootloader.bin', 'partition_table/partition-table.bin')
        manifest = {'version': '0.1.0', 'idf': '5.5.4', 'chip': 'esp32s3',
                    'secureBoot': False, 'flashEncryption': False, 'efuseProgramming': False,
                    'files': {name: {'sha256': hashlib.sha256((output / name).read_bytes()).hexdigest(),
                                     'size': (output / name).stat().st_size} for name in binaries}}
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        print(f'Firmware package: {output}', flush=True)
        subprocess.run([sys.executable, str(ROOT / 'tools/make-browser-image.py')], check=True)
    if args.flash or args.update:
        subprocess.run(command + ['-p', args.port, 'app-flash' if args.update else 'flash'], check=True)

if __name__ == '__main__':
    main()
