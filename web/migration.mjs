import {encodeBase32, normalizeRecord, MAX_RECORDS} from './otp.mjs';

// Google migration wire schema, also documented by Aegis:
// https://github.com/beemdevelopment/Aegis/blob/master/app/src/main/proto/google_auth.proto
const utf8 = new TextDecoder('utf-8', {fatal: true});
const malformed = () => new Error('Повреждён QR-код переноса Google Authenticator');

function fields(bytes) {
  let offset = 0;
  function varint() {
    let value = 0n;
    for (let i = 0; i < 10; i++) {
      if (offset >= bytes.length) throw malformed();
      const byte = bytes[offset++];
      if (i === 9 && byte > 1) throw malformed();
      value |= BigInt(byte & 127) << BigInt(i * 7);
      if (!(byte & 128)) return value;
    }
    throw malformed();
  }
  const result = [];
  while (offset < bytes.length) {
    const tag = varint(), field = Number(tag >> 3n), wire = Number(tag & 7n);
    if (field < 1 || field > 536870911) throw malformed();
    let value;
    if (wire === 0) value = varint();
    else {
      const size = wire === 2 ? Number(varint()) : wire === 1 ? 8 : wire === 5 ? 4 : -1;
      if (!Number.isSafeInteger(size) || size < 0 || size > bytes.length - offset) throw malformed();
      value = bytes.subarray(offset, offset + size); offset += size;
    }
    result.push({field, wire, value});
  }
  return result;
}

function message(bytes, schema, repeated = 0) {
  const result = new Map();
  for (const {field, wire, value} of fields(bytes)) {
    if (!Object.hasOwn(schema, field)) continue;
    if (wire !== schema[field] || (result.has(field) && field !== repeated)) throw malformed();
    if (field === repeated) {
      if (!result.has(field)) result.set(field, []);
      result.get(field).push(value);
      if (result.get(field).length > MAX_RECORDS) throw new Error('Лимит: 100 аккаунтов');
    } else result.set(field, value);
  }
  return result;
}

function int32(value) {
  value ??= 0n;
  const signed = BigInt.asIntN(32, value);
  if (BigInt.asUintN(64, signed) !== value && BigInt.asUintN(32, signed) !== value) throw malformed();
  return Number(signed);
}

function account(bytes) {
  const p = message(bytes, {1: 2, 2: 2, 3: 2, 4: 0, 5: 0, 6: 0, 7: 0});
  if (p.get(6) !== 2n) throw new Error('Поддерживаются только коды по времени (TOTP); HOTP переносить нельзя');
  const algorithm = {1: 'SHA1', 2: 'SHA256', 3: 'SHA512'}[String(p.get(4))];
  const digits = {1: 6, 2: 8}[String(p.get(5))];
  if (!algorithm || !digits) throw new Error('Неподдерживаемый алгоритм или число цифр');
  let name, issuer;
  try { name = utf8.decode(p.get(2) ?? new Uint8Array()).trim(); issuer = utf8.decode(p.get(3) ?? new Uint8Array()).trim(); }
  catch { throw new Error('Повреждено название аккаунта'); }
  if (issuer && name.startsWith(issuer + ':')) name = name.slice(issuer.length + 1).trim();
  else if (!issuer && name.includes(':')) {
    const colon = name.indexOf(':'); issuer = name.slice(0, colon).trim(); name = name.slice(colon + 1).trim();
  }
  // The migration format has no period field. Google TOTP exports use 30 seconds.
  return normalizeRecord({issuer, account: name, secret: encodeBase32(p.get(1) ?? []), algorithm, digits, period: 30});
}

export function clearSecrets(records) { for (const record of records) record.secret = ''; }

export function parseGoogleMigration(input) {
  if (typeof input !== 'string' || input.length > 65536) throw malformed();
  let url;
  try { url = new URL(input.trim()); } catch { throw malformed(); }
  if (url.protocol !== 'otpauth-migration:' || url.hostname !== 'offline' ||
      (url.pathname && url.pathname !== '/') || url.username || url.password || url.port || url.hash ||
      [...url.searchParams.keys()].some(k => k !== 'data') || url.searchParams.getAll('data').length !== 1) {
    throw new Error('Нужен QR-код из «Перенос аккаунтов → Экспорт аккаунтов» Google Authenticator');
  }
  const base64 = url.searchParams.get('data').replace(/ /g, '+');
  if (!base64 || !/^[A-Za-z0-9+/]*={0,2}$/.test(base64) || base64.length % 4 === 1) throw malformed();
  let bytes;
  try { bytes = Uint8Array.from(atob(base64), c => c.charCodeAt(0)); } catch { throw malformed(); }
  const records = [];
  try {
    const p = message(bytes, {1: 2, 2: 0, 3: 0, 4: 0, 5: 0}, 1);
    const version = int32(p.get(2)), batchSize = p.has(3) ? int32(p.get(3)) : 1;
    const batchIndex = int32(p.get(4)), batchId = int32(p.get(5));
    if (version !== 1) throw new Error('Эта версия экспорта Google Authenticator не поддерживается');
    if (batchSize < 1 || batchSize > MAX_RECORDS || batchIndex < 0 || batchIndex >= batchSize) throw malformed();
    for (const entry of p.get(1) ?? []) {
      try { records.push(account(entry)); }
      catch (error) { throw new Error(`Аккаунт №${records.length + 1}: ${error.message}`); }
    }
    if (!records.length) throw new Error('В QR-коде нет аккаунтов');
    return {records, version, batchSize, batchIndex, batchId};
  } catch (error) { clearSecrets(records); throw error; }
  finally { bytes.fill(0); }
}

export function recordKey(record) { return JSON.stringify([record.issuer, record.account]); }
const comparable = record => JSON.stringify([record.issuer, record.account, record.secret, record.algorithm, record.digits, record.period]);

export class MigrationBatch {
  constructor() { this.parts = new Map(); this.metadata = null; }
  get records() { return [...this.parts].sort(([a], [b]) => a - b).flatMap(([, part]) => part.records); }
  get complete() { return Boolean(this.metadata && this.parts.size === this.metadata.batchSize); }
  add(uri) {
    const part = parseGoogleMigration(uri);
    try {
      if (this.metadata && ['version', 'batchSize', 'batchId'].some(k => part[k] !== this.metadata[k])) {
        throw new Error('Это QR-код другого экспорта. Очистите список или отсканируйте остальные QR текущего экспорта.');
      }
      const existing = this.parts.get(part.batchIndex);
      if (existing) {
        if (JSON.stringify(existing.records.map(comparable)) !== JSON.stringify(part.records.map(comparable))) throw new Error('QR-коды с одинаковым номером содержат разные аккаунты');
        clearSecrets(part.records); return false;
      }
      if (this.records.length + part.records.length > MAX_RECORDS) throw new Error('Лимит: 100 аккаунтов');
      this.metadata ??= {version: part.version, batchSize: part.batchSize, batchId: part.batchId};
      this.parts.set(part.batchIndex, part); return true;
    } catch (error) { clearSecrets(part.records); throw error; }
  }
  clear() { clearSecrets(this.records); this.parts.clear(); this.metadata = null; }
}

export function selectMigrationRecords(batch, selectedIds, existing) {
  if (!batch.complete) throw new Error('Сначала прочитайте все QR-коды этого экспорта');
  const result = batch.records.filter(record => selectedIds.has(record.id));
  if (!result.length) throw new Error('Выберите хотя бы один аккаунт');
  const seen = new Set(existing.map(recordKey));
  for (const record of result) {
    if (seen.has(recordKey(record))) throw new Error('Аккаунт с таким сервисом и именем уже есть в списке или на устройстве');
    seen.add(recordKey(record));
  }
  if (existing.length + result.length > MAX_RECORDS) throw new Error(`Недостаточно места: свободно ${MAX_RECORDS - existing.length}, выбрано ${result.length}`);
  return result;
}
