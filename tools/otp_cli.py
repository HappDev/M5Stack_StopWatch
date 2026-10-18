"""Local USB client for StopWatch OTP v1. Never accepts secrets on the command line."""
import argparse
import base64
import getpass
import json
from pathlib import Path
import re
import sys
import time
from urllib.parse import parse_qsl, unquote, urlsplit
import uuid
import warnings

MAX_FRAME = 4096
ERRORS = {
    'LOCKED': 'Введите PIN и включите «Разрешить USB» на StopWatch.',
    'CANCELLED': 'Операция отклонена на устройстве.',
    'BUSY': 'На устройстве уже ожидается подтверждение другой операции.',
    'INPUT': 'Устройство отклонило параметры операции.',
    'FAILED': 'Устройство не смогло сохранить данные. Проверьте его экран.',
    'STATE': 'Операция недоступна в текущем состоянии устройства.',
    'FRAME': 'Устройство отклонило сообщение протокола.',
    'UNKNOWN': 'Эта команда не поддерживается установленной прошивкой.',
}

class CliError(Exception):
    pass

class RequestTimeout(CliError):
    pass

class DeviceError(CliError):
    def __init__(self, code):
        self.code = code
        super().__init__(ERRORS.get(code, 'Ошибка устройства. Проверьте его экран.'))

def strict_json(text):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('Повторяющиеся поля JSON')
            result[key] = value
        return result
    def bad_constant(_):
        raise ValueError('Недопустимое число JSON')
    try:
        return json.loads(text, object_pairs_hook=pairs, parse_constant=bad_constant)
    except (ValueError, UnicodeError, RecursionError):
        raise CliError('Некорректный JSON; содержимое не выводится.') from None

def normalize_record(value):
    if not isinstance(value, dict):
        raise CliError('Ожидается запись аккаунта.')
    record = {'id': value.get('id', str(uuid.uuid4()))}
    if not isinstance(record['id'], str) or not re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', record['id']):
        raise CliError('Некорректный UUID аккаунта.')
    for field, limit in (('issuer', 96), ('account', 128)):
        label = value.get(field, '')
        try:
            valid = isinstance(label, str) and not re.search(r'[\x00-\x1f\x7f]', label) and len(label.encode('utf-8')) <= limit
        except UnicodeError:
            valid = False
        if not valid:
            raise CliError(f'Недопустимое поле {field}: ограничение {limit} байт UTF-8.')
        record[field] = label.strip()
    if not record['issuer'] and not record['account']:
        raise CliError('Укажите сервис или аккаунт.')
    for field, default, choices in (('algorithm', 'SHA1', ('SHA1', 'SHA256', 'SHA512')),
                                     ('digits', 6, (6, 8)), ('period', 30, (30, 60))):
        item = value.get(field, default)
        if item not in choices or (field != 'algorithm' and type(item) is not int):
            raise CliError(f'Неподдерживаемый параметр {field}.')
        record[field] = item
    secret = value.get('secret')
    if not isinstance(secret, str) or len(secret) > 512:
        raise CliError('Неверный секрет Base32.')
    secret = ''.join(secret.split()).upper()
    match = re.fullmatch(r'([A-Z2-7]+)(=*)', secret)
    if not match:
        raise CliError('Секрет Base32 должен содержать A–Z и 2–7.')
    raw, padding = match.groups()
    needed = (-len(raw)) % 8
    if len(raw) % 8 in (1, 3, 6) or (padding and len(padding) != needed):
        raise CliError('Неверная длина или padding Base32.')
    try:
        decoded = base64.b32decode(raw + '=' * needed)
    except ValueError:
        raise CliError('Неверный секрет Base32.') from None
    if not 10 <= len(decoded) <= 128 or base64.b32encode(decoded).decode().rstrip('=') != raw:
        raise CliError('Секрет должен содержать 10–128 байт с корректными завершающими битами Base32.')
    record['secret'] = raw
    return record

def parse_uri(text):
    if not isinstance(text, str) or len(text) > 4096:
        raise CliError('Некорректная ссылка otpauth.')
    try:
        text = text.strip()
        if re.search(r'[\x00-\x1f\x7f]|%(?![0-9a-fA-F]{2})', text):
            raise ValueError()
        url = urlsplit(text)
        if url.scheme != 'otpauth' or url.netloc != 'totp' or url.fragment:
            raise ValueError()
        items = parse_qsl(url.query, keep_blank_values=True, strict_parsing=True, errors='strict', max_num_fields=10)
        values = dict(items)
        if len(values) != len(items) or set(values) - {'issuer', 'secret', 'algorithm', 'digits', 'period'}:
            raise ValueError()
        label = unquote(url.path.removeprefix('/'), errors='strict')
        prefix, separator, account = label.partition(':')
        issuer = values.get('issuer', prefix.strip() if separator else '')
        if separator and prefix.strip() and issuer != prefix.strip():
            raise ValueError()
        for field in ('digits', 'period'):
            if field in values:
                if not re.fullmatch(r'[0-9]{1,4}', values[field]):
                    raise ValueError()
                values[field] = int(values[field])
        values.update(issuer=issuer, account=account.strip() if separator else label)
    except (ValueError, UnicodeError):
        raise CliError('Поддерживаются стандартные otpauth://totp без повторяющихся или неизвестных параметров.') from None
    return normalize_record(values)

def read_records(path):
    with Path(path).open('rb') as source:
        data = source.read(256 * 1024 + 1)
    if len(data) > 256 * 1024:
        raise CliError('Файл импорта больше 256 КиБ.')
    value = strict_json(data)
    if isinstance(value, dict) and value.get('format') == 'stopwatch-otp':
        raise CliError('Зашифрованные .otpvault восстанавливаются через сайт; здесь нужен JSON со списком записей.')
    if isinstance(value, dict):
        value = value.get('records')
    if not isinstance(value, list) or not 1 <= len(value) <= 100:
        raise CliError('В JSON должен быть массив из 1–100 записей или объект с полем records.')
    records = [normalize_record(row) for row in value]
    if len({row['id'] for row in records}) != len(records):
        raise CliError('В файле повторяются UUID.')
    return records

class Device:
    def __init__(self, port, timeout=60, serial_factory=None):
        self.port_name, self.timeout, self.serial_factory = port, timeout, serial_factory
        self.serial, self.next_id = None, 1

    def __enter__(self):
        if self.serial_factory is None:
            try:
                import serial
            except ImportError:
                raise CliError('Установите зависимость: python3 -m pip install -r tools/requirements-usb.txt') from None
            self.serial_factory = serial.Serial
        try:
            self.serial = self.serial_factory(port=None, baudrate=115200, timeout=0.1, write_timeout=3)
            self.serial.dtr = False
            self.serial.rts = False
            self.serial.port = self.port_name
            self.serial.open()
            self.serial.reset_input_buffer()
            self.serial.dtr = True
            # The firmware discards queued input on a DTR transition. Retry only
            # the read-only handshake, never an account modification.
            for attempt in range(3):
                try:
                    self.request('status', timeout=1.5)
                    break
                except RequestTimeout:
                    if attempt == 2:
                        raise
        except OSError:
            self.close()
            raise CliError('Не удалось открыть USB-порт. Закройте ESPHome Web, сайт и Serial Monitor; проверьте USB в Parallels.') from None
        except BaseException:
            self.close()
            raise
        return self

    def close(self):
        if self.serial is not None:
            try:
                if self.serial.is_open:
                    self.serial.dtr = False
            except OSError:
                pass
            finally:
                try:
                    self.serial.close()
                except OSError:
                    pass
                finally:
                    self.serial = None

    def __exit__(self, *_):
        self.close()

    def request(self, op, args=None, timeout=None):
        request_id = self.next_id
        self.next_id += 1
        frame = bytearray((json.dumps({'v':1, 'id':request_id, 'op':op, 'args':args or {}},
                           ensure_ascii=False, separators=(',', ':'), allow_nan=False) + '\n').encode('utf-8'))
        received = bytearray()
        if len(frame) > MAX_FRAME:
            frame[:] = b'\0' * len(frame)
            raise CliError('Запрос превышает 4 КиБ.')
        try:
            if self.serial.write(frame) != len(frame):
                raise CliError('USB-запрос передан не полностью. Не повторяйте изменение без проверки списка.')
            deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
            while time.monotonic() < deadline:
                part = self.serial.read(1)
                if not part:
                    continue
                received.extend(part)
                if len(received) > MAX_FRAME or b'\0' in part:
                    raise CliError('Повреждён или слишком велик ответ устройства.')
                if part != b'\n':
                    continue
                if received in (b'\n', b'\r\n'):
                    received.clear()
                    continue
                response = strict_json(received)
                received[:] = b'\0' * len(received)
                received.clear()
                if not isinstance(response, dict) or type(response.get('id')) is not int:
                    raise CliError('Некорректный ответ устройства.')
                if response['id'] == 0:
                    raise DeviceError('FRAME')
                if response['id'] != request_id:
                    continue
                if type(response.get('v')) is not int or response['v'] != 1 or type(response.get('ok')) is not bool:
                    raise CliError('Несовместимый протокол. Нужна прошивка StopWatch OTP.')
                if not response['ok']:
                    error = response.get('error')
                    code = error.get('code') if isinstance(error, dict) else None
                    raise DeviceError(code if isinstance(code, str) else None)
                if not isinstance(response.get('result'), dict):
                    raise CliError('Некорректный результат устройства.')
                return response['result']
            raise RequestTimeout('Нет ответа StopWatch. Проверьте рабочую прошивку и режим USB. После тайм-аута изменения проверьте список: оно могло сохраниться. Автоматического повтора нет.')
        except OSError:
            raise CliError('USB отключён или недоступен. После изменения проверьте список перед повтором.') from None
        finally:
            frame[:] = b'\0' * len(frame)
            received[:] = b'\0' * len(received)

    def wait_authorized(self, wait=120):
        print('На StopWatch введите PIN, затем «Настройки» → «Разрешить USB». Порт остаётся открытым.', flush=True)
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            state = self.request('status', timeout=min(3, max(0.1, deadline - time.monotonic())))
            if state.get('state') == 'unlocked' and state.get('authorized') is True:
                return state
            if state.get('state') in ('damaged', 'storage_error', 'empty'):
                raise CliError('Сначала создайте PIN или устраните ошибку хранилища на экране StopWatch.')
            time.sleep(0.5)
        raise CliError('Не получено разрешение USB на StopWatch.')

    def list_records(self):
        rows = []
        for offset in range(0, 100, 5):
            page = self.request('list', {'offset': offset})
            total, records = page.get('total'), page.get('records')
            if (type(total) is not int or not 0 <= total <= 100 or not isinstance(records, list)
                    or len(records) != min(5, max(0, total - offset))
                    or any(not isinstance(row, dict) or 'secret' in row for row in records)):
                raise CliError('Некорректный список аккаунтов.')
            rows.extend(records)
            if len(rows) == total:
                return rows
        raise CliError('Не удалось прочитать полный список.')

def hidden_input(prompt):
    # Refuse getpass's echo fallback; secrets must not appear in terminal output.
    with warnings.catch_warnings():
        warnings.simplefilter('error', getpass.GetPassWarning)
        try:
            return getpass.getpass(prompt)
        except getpass.GetPassWarning:
            raise CliError('Для скрытого ввода откройте обычный Терминал/PowerShell, без перенаправления ввода.') from None

def main(argv=None):
    parser = argparse.ArgumentParser(description='StopWatch OTP: аккаунты по USB без браузера. PIN вводится только на устройстве.')
    parser.add_argument('--port', help='COM7 (Windows) или /dev/cu.usbmodem... (Mac)')
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('ports', 'status', 'list', 'time', 'lock'):
        sub.add_parser(command)
    add = sub.add_parser('add', help='Добавить аккаунт; секрет вводится скрыто')
    add.add_argument('--issuer')
    add.add_argument('--account')
    add.add_argument('--algorithm', choices=('SHA1', 'SHA256', 'SHA512'), default='SHA1')
    add.add_argument('--digits', type=int, choices=(6, 8), default=6)
    add.add_argument('--period', type=int, choices=(30, 60), default=30)
    batch = sub.add_parser('import', help='Добавить набор из локального JSON без удаления текущих аккаунтов')
    batch.add_argument('file', type=Path)
    args = parser.parse_args(argv)
    if args.command == 'ports':
        try:
            from serial.tools import list_ports
        except ImportError:
            raise CliError('Установите зависимость: python3 -m pip install -r tools/requirements-usb.txt') from None
        ports = list(list_ports.comports())
        for port in ports:
            print(f'{port.device}  {port.description}')
        if not ports:
            print('USB-порты не найдены. Проверьте кабель и передачу USB в macOS/Windows в Parallels.')
        return 0
    if not args.port:
        parser.error('Укажите --port перед командой; список портов: python3 tools/otp_cli.py ports')
    records = []
    if args.command == 'add':
        value = hidden_input('Вставьте otpauth://totp/... или секрет Base32 (ввод скрыт): ').strip()
        if value.startswith('otpauth:'):
            records = [parse_uri(value)]
        else:
            records = [normalize_record({'issuer':args.issuer if args.issuer is not None else input('Сервис: '),
                                         'account':args.account if args.account is not None else input('Аккаунт: '),
                                         'secret':value, 'algorithm':args.algorithm, 'digits':args.digits, 'period':args.period})]
        value = None
    elif args.command == 'import':
        records = read_records(args.file)
    try:
        with Device(args.port) as device:
            if args.command in ('status', 'lock'):
                result = device.request(args.command, timeout=5)
                if args.command == 'status':
                    for field in ('firmware', 'state', 'authorized', 'timeValid', 'time', 'batteryMv', 'retryAfter'):
                        print(f'{field}: {result.get(field)}')
                else:
                    print('Устройство заблокировано.')
                return 0
            state = device.wait_authorized()
            if state.get('timeValid') is False and args.command in ('add', 'import'):
                print('Часы ещё не установлены: импорт работает, но для TOTP затем выполните команду time.', flush=True)
            if args.command == 'list':
                rows = device.list_records()
                for row in rows:
                    print(f"{row.get('id')}  {row.get('issuer')} · {row.get('account')}  {row.get('algorithm')}/{row.get('digits')}/{row.get('period')}")
                print(f'Аккаунтов: {len(rows)}; секреты не запрашивались.')
            elif args.command == 'time':
                print('Подтвердите установку времени UTC на StopWatch.', flush=True)
                device.request('time_set', {'epoch':int(time.time())})
                print('Время установлено.')
            else:
                current = device.list_records()
                new_ids = {r['id'] for r in records} - {r['id'] for r in current}
                if len(current) + len(new_ids) > 100:
                    raise CliError('Для набора не хватает места: лимит 100 аккаунтов. Импорт не начат.')
                saved = 0
                for record in records:
                    print(f"Подтвердите на StopWatch: {record['issuer']} · {record['account']} ({record['algorithm']}/{record['digits']}/{record['period']}).", flush=True)
                    device.request('upsert', {'record': record})
                    saved += 1
                    print(f"Сохранено {saved}/{len(records)}; UUID: {record['id']}", flush=True)
                print('Готово. Обновите резервную копию. USB-разрешение будет сброшено при закрытии порта.')
    finally:
        for record in records:
            record['secret'] = ''
        records.clear()
    return 0

if __name__ == '__main__':
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='replace')
    try:
        raise SystemExit(main())
    except (CliError, OSError, EOFError) as error:
        # Do not print tracebacks or arbitrary exception payloads that may contain inputs.
        print('Ошибка: ' + (str(error) if isinstance(error, CliError) else 'Не удалось прочитать ввод/файл или открыть USB.'), file=sys.stderr)
        raise SystemExit(1)
    except KeyboardInterrupt:
        print('\nОтменено. Перед повтором операции проверьте список аккаунтов.', file=sys.stderr)
        raise SystemExit(130)
