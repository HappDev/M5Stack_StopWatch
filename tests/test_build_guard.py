import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location('otp_build', TOOLS / 'build.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class BuildGuardTests(unittest.TestCase):
    base = 'CONFIG_ESP_CONSOLE_NONE=y\nCONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT=y\nCONFIG_LOG_DEFAULT_LEVEL_NONE=y\n'
    def check_config(self, text):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / 'sdkconfig'
            config.write_text(text, encoding='utf-8')
            module.verify_configuration(config)

    def test_expected_configuration(self):
        self.check_config(self.base + 'CONFIG_SECURE_BOOT_V2_PREFERRED=y\nCONFIG_SECURE_BOOT_V2_RSA_SUPPORTED=y\n# CONFIG_SECURE_BOOT is not set\n')

    def test_refuses_irreversible_settings_and_memory_dumps(self):
        base = self.base
        for option in ('CONFIG_SECURE_BOOT', 'CONFIG_SECURE_BOOT_V2_ENABLED', 'CONFIG_SECURE_FLASH_ENC_ENABLED', 'CONFIG_NVS_ENCRYPTION', 'CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK', 'CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH', 'CONFIG_ESP_COREDUMP_ENABLE_TO_UART'):
            with self.subTest(option=option), self.assertRaises(SystemExit):
                self.check_config(base + option + '=y\n')
        with self.assertRaises(SystemExit):
            self.check_config('CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y\n')

if __name__ == '__main__':
    unittest.main()
