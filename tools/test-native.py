"""Run the actual firmware crypto/vault code with simulated Flash/NVS and mbedTLS.
Requires a host C++ compiler (Zig 0.14.1 supported) and the ESP-IDF source tree.
No OTP implementations are duplicated in this test harness.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--zig', required=True)
    parser.add_argument('--idf', default=os.environ.get('IDF_PATH'))
    args = parser.parse_args()
    if not args.idf:
        raise SystemExit('Specify --idf PATH or IDF_PATH')
    mbed = Path(args.idf) / 'components/mbedtls/mbedtls'
    sources = ['aes', 'aesni', 'aesce', 'cipher', 'cipher_wrap', 'constant_time', 'gcm', 'md', 'pkcs5', 'platform', 'platform_util', 'sha1', 'sha256', 'sha512']
    with tempfile.TemporaryDirectory(prefix='stopwatch-native-', ignore_cleanup_errors=True) as build:
        out = Path(build) / ('tests.exe' if os.name == 'nt' else 'tests')
        flags = ['-O2', '-Wno-deprecated-declarations', '-I'+str(ROOT / 'tests/native'),
                 '-I'+str(ROOT / 'firmware/main'), '-I'+str(mbed / 'include'),
                 '-I'+str(Path(args.idf) / 'components/json/cJSON'),
                 '-DMBEDTLS_CONFIG_FILE="host_config.h"']
        objects = []
        for name in sources:
            print(f'Compile mbedTLS: {name}', flush=True)
            obj = Path(build) / (name + '.o')
            subprocess.run([args.zig, 'cc', *flags, '-c', str(mbed / 'library' / (name + '.c')), '-o', str(obj)], check=True)
            objects.append(str(obj))
        obj = Path(build) / 'cJSON.o'
        subprocess.run([args.zig, 'cc', *flags, '-c', str(Path(args.idf) / 'components/json/cJSON/cJSON.c'), '-o', str(obj)], check=True)
        objects.append(str(obj))
        cmd = [args.zig, 'c++', *flags, '-UNDEBUG', '-std=c++17', str(ROOT / 'firmware/main/crypto.cpp'),
               str(ROOT / 'firmware/main/vault.cpp'), str(ROOT / 'firmware/main/protocol.cpp'),
               str(ROOT / 'tests/native/main.cpp'), *objects, '-o', str(out)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(out)], check=True)

if __name__ == '__main__':
    main()
