export const MAX_RECORDS = 100;
const encoder = new TextEncoder();
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
export function decodeBase32(input) {
  if (typeof input !== 'string' || input.length > 512) throw new Error('Неверный секрет Base32');
  const text = input.replace(/\s/g, '').toUpperCase();
  const match = /^([A-Z2-7]+)(=*)$/.exec(text);
  if (!match) throw new Error('Секрет должен содержать символы A–Z и 2–7');
  const [, raw, padding] = match;
  if ([1, 3, 6].includes(raw.length % 8) || (padding.length &&
      ((raw.length + padding.length) % 8 || padding.length !== (8 - raw.length % 8) % 8))) {
    throw new Error('Неверная длина Base32');
  }
  const bytes = []; let buffer = 0; let bits = 0;
  for (const char of raw) {
    const value = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'.indexOf(char);
    buffer = (buffer << 5) | value; bits += 5;
    if (bits >= 8) { bits -= 8; bytes.push((buffer >>> bits) & 255); }
  }
  if (bits && (buffer & ((1 << bits) - 1))) throw new Error('Неверные завершающие биты Base32');
  return new Uint8Array(bytes);
}
export function encodeBase32(bytes) {
  let result = ''; let buffer = 0; let bits = 0;
  for (const byte of bytes) {
    buffer = (buffer << 8) | byte; bits += 8;
    while (bits >= 5) { bits -= 5; result += 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'[(buffer >>> bits) & 31]; }
  }
  if (bits) result += 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'[(buffer << (5 - bits)) & 31];
  return result;
}
function label(value, max, field) {
  if (typeof value !== 'string' || /[\x00-\x1f\x7f]/.test(value) || encoder.encode(value).length > max) {
    throw new Error(`Недопустимое поле «${field}»`);
  }
  return value.trim();
}
export function normalizeRecord(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Неверная запись');
  const id = value.id ?? crypto.randomUUID();
  if (typeof id !== 'string' || !UUID.test(id)) throw new Error('Неверный идентификатор аккаунта');
  const issuer = label(value.issuer ?? '', 96, 'Сервис');
  const account = label(value.account ?? '', 128, 'Аккаунт');
  if (!issuer && !account) throw new Error('Укажите сервис или аккаунт');
  const algorithm = value.algorithm ?? 'SHA1';
  const digits = value.digits ?? 6;
  const period = value.period ?? 30;
  if (!['SHA1', 'SHA256', 'SHA512'].includes(algorithm)) throw new Error('Поддерживаются SHA1, SHA256 и SHA512');
  if (![6, 8].includes(digits)) throw new Error('Код должен содержать 6 или 8 цифр');
  if (![30, 60].includes(period)) throw new Error('Период должен быть 30 или 60 секунд');
  const bytes = decodeBase32(value.secret);
  try {
    if (bytes.length < 10 || bytes.length > 128) throw new Error('Секрет должен содержать 10–128 байт');
    return {id, issuer, account, secret: encodeBase32(bytes), algorithm, digits, period};
  } finally { bytes.fill(0); }
}
export function parseOtpAuth(input) {
  let url;
  try { url = new URL(input.trim()); } catch { throw new Error('Некорректная ссылка otpauth://'); }
  if (url.protocol !== 'otpauth:' || url.hostname !== 'totp' || url.username || url.password || url.port || url.hash) {
    throw new Error('Поддерживаются только стандартные ссылки otpauth://totp');
  }
  const allowed = new Set(['secret', 'issuer', 'algorithm', 'digits', 'period']);
  for (const key of url.searchParams.keys()) {
    if (!allowed.has(key) || url.searchParams.getAll(key).length !== 1) throw new Error(`Неподдерживаемый параметр: ${key}`);
  }
  let name;
  try { name = decodeURIComponent(url.pathname.slice(1)); } catch { throw new Error('Неверное название аккаунта'); }
  const colon = name.indexOf(':');
  const prefix = colon >= 0 ? name.slice(0, colon).trim() : '';
  const issuer = url.searchParams.get('issuer') ?? prefix;
  if (prefix && prefix !== issuer) throw new Error('Название сервиса в ссылке не совпадает с issuer');
  function number(key, fallback) {
    const text = url.searchParams.get(key);
    if (text === null) return fallback;
    if (!/^\d+$/.test(text)) throw new Error(`Неверный параметр ${key}`);
    return Number(text);
  }
  return normalizeRecord({issuer, account: colon >= 0 ? name.slice(colon + 1).trim() : name,
    secret: url.searchParams.get('secret'), algorithm: url.searchParams.get('algorithm') ?? 'SHA1',
    digits: number('digits', 6), period: number('period', 30)});
}
export function validateRecords(records) {
  if (!Array.isArray(records) || records.length > MAX_RECORDS) throw new Error('Лимит: 100 аккаунтов');
  const normalized = records.map(normalizeRecord);
  if (new Set(normalized.map(r => r.id)).size !== normalized.length) throw new Error('В копии повторяются идентификаторы');
  return normalized;
}
