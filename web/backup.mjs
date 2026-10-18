import {validateRecords} from './otp.mjs';
const ITERATIONS = 600000;
const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', {fatal: true});
function b64(bytes) {
  let s = ''; for (const b of bytes) s += String.fromCharCode(b); return btoa(s);
}
function unb64(s, length) {
  if (typeof s !== 'string' || s.length > 180000 || !/^[A-Za-z0-9+/]*={0,2}$/.test(s) || s.length % 4) throw new Error('Неверный формат файла');
  const bytes = Uint8Array.from(atob(s), c => c.charCodeAt(0));
  if (b64(bytes) !== s || (length && bytes.length !== length)) throw new Error('Неверный формат файла');
  return bytes;
}
function header(value) {
  return {format: 'stopwatch-otp', version: 1, kdf: 'PBKDF2-SHA256', iterations: ITERATIONS,
    salt: value.salt, cipher: 'AES-256-GCM', nonce: value.nonce};
}
async function key(password, salt, usage) {
  if (typeof password !== 'string' || password.length < 16 || encoder.encode(password).length > 1024) {
    throw new Error('Парольная фраза: от 16 символов, не более 1024 байт');
  }
  const bytes = encoder.encode(password);
  try {
    const material = await crypto.subtle.importKey('raw', bytes, 'PBKDF2', false, ['deriveKey']);
    return await crypto.subtle.deriveKey({name: 'PBKDF2', hash: 'SHA-256', iterations: ITERATIONS, salt},
      material, {name: 'AES-GCM', length: 256}, false, [usage]);
  } finally { bytes.fill(0); }
}
export async function encryptBackup(records, password) {
  const safe = validateRecords(records);
  const salt = crypto.getRandomValues(new Uint8Array(16));
  const nonce = crypto.getRandomValues(new Uint8Array(12));
  const meta = header({salt: b64(salt), nonce: b64(nonce)});
  const bytes = encoder.encode(JSON.stringify({version: 1, records: safe}));
  try {
    const aes = await key(password, salt, 'encrypt');
    const ciphertext = await crypto.subtle.encrypt({name: 'AES-GCM', iv: nonce, tagLength: 128,
      additionalData: encoder.encode(JSON.stringify(meta))}, aes, bytes);
    return JSON.stringify({...meta, ciphertext: b64(new Uint8Array(ciphertext))}, null, 2);
  } finally { bytes.fill(0); for (const r of safe) r.secret = ''; }
}
export async function decryptBackup(text, password) {
  if (typeof text !== 'string' || encoder.encode(text).length > 256 * 1024) throw new Error('Файл слишком большой');
  let value;
  try { value = JSON.parse(text); } catch { throw new Error('Неверный формат файла'); }
  if (!value || value.format !== 'stopwatch-otp' || value.version !== 1 || value.kdf !== 'PBKDF2-SHA256' ||
      value.iterations !== ITERATIONS || value.cipher !== 'AES-256-GCM' ||
      Object.keys(value).some(k => !['format','version','kdf','iterations','salt','cipher','nonce','ciphertext'].includes(k))) {
    throw new Error('Неподдерживаемая версия или параметры копии');
  }
  const salt = unb64(value.salt, 16), nonce = unb64(value.nonce, 12), ciphertext = unb64(value.ciphertext);
  if (ciphertext.length < 16) throw new Error('Повреждённый файл');
  const aes = await key(password, salt, 'decrypt');
  let bytes;
  try {
    bytes = new Uint8Array(await crypto.subtle.decrypt({name: 'AES-GCM', iv: nonce, tagLength: 128,
      additionalData: encoder.encode(JSON.stringify(header(value)))}, aes, ciphertext));
  } catch { throw new Error('Неверная парольная фраза или повреждённая копия'); }
  try {
    const data = JSON.parse(decoder.decode(bytes));
    if (data.version !== 1) throw new Error('Неподдерживаемая версия данных');
    return validateRecords(data.records);
  } finally { bytes.fill(0); ciphertext.fill(0); }
}
