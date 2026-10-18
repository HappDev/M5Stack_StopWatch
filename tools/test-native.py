"""Run the actual firmware crypto/vault code with simulated Flash/NVS and mbedTLS.
Requires a host C++ compiler (Zig 0.14.1 supported) and the ESP-IDF source tree.
No OTP implementations are duplicated in this test harness.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--zig', required=True)
    parser.add_argument('--idf', default=os.environ.get('IDF_PATH'))
    parser.add_argument('--ui', action='store_true', help='Run the real LVGL/UI suite instead of the storage/protocol suite')
    args = parser.parse_args()
    if not args.idf:
        raise SystemExit('Specify --idf PATH or IDF_PATH')
    mbed = Path(args.idf) / 'components/mbedtls/mbedtls'
    sources = ['aes', 'aesni', 'aesce', 'cipher', 'cipher_wrap', 'constant_time', 'gcm', 'md', 'pkcs5', 'platform', 'platform_util', 'sha1', 'sha256', 'sha512']
    with tempfile.TemporaryDirectory(prefix='stopwatch-native-', ignore_cleanup_errors=True) as build:
        out = Path(build) / ('tests.exe' if os.name == 'nt' else 'tests')
        flags = ['-O2', '-Wno-deprecated-declarations', '-I'+str(ROOT / 'tests/native'),
                 '-I'+str(Path(args.idf) / 'components/esp_hid/include'),
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
        obj = Path(build) / 'esp_hid_common.o'
        subprocess.run([args.zig, 'cc', *flags, '-c', str(Path(args.idf) / 'components/esp_hid/src/esp_hid_common.c'), '-o', str(obj)], check=True)
        objects.append(str(obj))
        ui_objects = []
        if args.ui:
            lvgl = ROOT / 'firmware/components/lvgl'
            if not lvgl.is_dir():
                raise SystemExit('Run tools/bootstrap.py first to obtain the pinned LVGL sources')
            # Resolve the real LVGL header before tests/native/lvgl.h. Rendering
            # and input dispatch are real; only board hardware is simulated.
            flags = ['-I'+str(lvgl), *flags, '-DLV_CONF_SKIP=1', '-DLV_KCONFIG_IGNORE=1',
                     '-DLV_LVGL_H_INCLUDE_SIMPLE=1', '-DLV_USE_STDLIB_MALLOC=1',
                     '-DLV_FONT_MONTSERRAT_28=1', '-DLV_FONT_MONTSERRAT_48=1']
            lv_sources = sorted((lvgl / 'src').rglob('*.c')) + [ROOT / 'firmware/main/font_ru.c']
            def compile_lvgl(item):
                index, source = item
                obj = Path(build) / f'lv{index}.o'
                subprocess.run([args.zig, 'cc', *flags, '-c', str(source), '-o', str(obj)], check=True)
                return str(obj)
            print(f'Compile real LVGL: {len(lv_sources)} sources', flush=True)
            with ThreadPoolExecutor(max_workers=6) as workers:
                lv_objects = list(workers.map(compile_lvgl, enumerate(lv_sources)))
            response = Path(build) / 'lvgl.rsp'
            response.write_text('\n'.join('"'+p.replace('\\', '/')+'"' for p in lv_objects), encoding='utf-8')
            library = Path(build) / 'lvgl.a'
            subprocess.run([args.zig, 'ar', 'rcs', str(library), '@'+str(response)], check=True)
            ui_objects = [str(ROOT / 'firmware/main/ui.cpp'), str(library)]
        cmd = [args.zig, 'c++', *flags, '-UNDEBUG', '-std=c++17', str(ROOT / 'firmware/main/crypto.cpp'),
               str(ROOT / 'firmware/main/ble_keyboard.cpp'),
               str(ROOT / 'firmware/main/pattern.cpp'),
               str(ROOT / 'firmware/main/clock.cpp'),
               str(ROOT / 'firmware/main/vault.cpp'), str(ROOT / 'firmware/main/protocol.cpp'),
               str(ROOT / ('tests/native/ui_main.cpp' if args.ui else 'tests/native/main.cpp')),
               *ui_objects, *objects, '-o', str(out)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(out)], check=True)

if __name__ == '__main__':
    main()
