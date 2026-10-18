"""Fetch pinned hardware/UI components. Does not install or flash firmware."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()

def main():
    dependencies = json.loads((ROOT / 'dependencies.json').read_text())
    for name, spec in dependencies.items():
        target = ROOT / 'firmware' / 'components' / name
        if not target.exists():
            run('git', 'clone', '--depth', '1', '--branch', spec['ref'], spec['url'], str(target))
        actual = run('git', '-c', f'safe.directory={target.as_posix()}', 'rev-parse', 'HEAD', cwd=target)
        if actual != spec['commit']:
            raise SystemExit(f'{name}: wrong commit {actual}, expected {spec["commit"]}')
        # Both translation units and consumers must see the same optional I2C ABI.
        # Native i2c_master handles are used; i2c_bus is never initialized.
        if name in ('M5PM1', 'M5IOE1'):
            cmake = target / 'CMakeLists.txt'
            text = cmake.read_text(encoding='utf-8')
            patch = 'idf_component_optional_requires(PUBLIC M5GFX)'
            if patch not in text:
                cmake.write_text(text + '\n' + patch + '\n', encoding='utf-8')
        print(f'{name}: {actual}')
    qr = ROOT / 'web' / 'vendor' / 'jsQR.js'
    if hashlib.sha256(qr.read_bytes()).hexdigest() != 'bc40c8a15196236b2314db0856f72ca0b49980cd5413b8c852a7349f5fee0859':
        raise SystemExit('Local jsQR integrity check failed')

if __name__ == '__main__':
    main()
