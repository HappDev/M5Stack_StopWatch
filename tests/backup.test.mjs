import test from 'node:test';
import assert from 'node:assert/strict';
import {encryptBackup,decryptBackup} from '../web/backup.mjs';
import {normalizeRecord} from '../web/otp.mjs';
const phrase='Separate backup phrase / 2026';
const r=normalizeRecord({issuer:'PrivateMail',account:'alice@example.com',secret:'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ'});
test('portable encrypted roundtrip independent of device PIN',async()=>{
  const file=await encryptBackup([r],phrase);
  assert(!file.includes(r.secret));assert(!file.includes(r.issuer));assert(!file.includes(r.account));
  assert.deepEqual(await decryptBackup(file,phrase),[r]);
  assert.deepEqual(await decryptBackup(await encryptBackup([],phrase),phrase),[]);
});
test('100 accounts, fresh salt and fresh nonce',async()=>{
  const rows=Array.from({length:100},(_,i)=>({...r,id:crypto.randomUUID(),account:`account-${i}`}));
  const a=await encryptBackup(rows,phrase),b=await encryptBackup(rows,phrase);
  assert.notEqual(JSON.parse(a).salt,JSON.parse(b).salt);assert.notEqual(JSON.parse(a).nonce,JSON.parse(b).nonce);
  assert.deepEqual(await decryptBackup(a,phrase),rows);
});
test('wrong password, altered ciphertext and altered authenticated headers fail',async()=>{
  const file=await encryptBackup([r],phrase);
  await assert.rejects(decryptBackup(file,phrase+'wrong'));
  for(const key of ['ciphertext','nonce','salt']){
    const obj=JSON.parse(file);const bytes=Buffer.from(obj[key],'base64');bytes[0]^=1;obj[key]=bytes.toString('base64');
    await assert.rejects(decryptBackup(JSON.stringify(obj),phrase),key);
  }
  const value=JSON.parse(file);value.iterations=1;await assert.rejects(decryptBackup(JSON.stringify(value),phrase));
  value.iterations=600000;value.version=2;await assert.rejects(decryptBackup(JSON.stringify(value),phrase));
});
test('reject short passwords and hostile files before KDF',async()=>{
  await assert.rejects(encryptBackup([r],'short'));
  await assert.rejects(decryptBackup('x'.repeat(256*1024+1),phrase));
  await assert.rejects(decryptBackup('{}',phrase));
  await assert.rejects(decryptBackup('not json',phrase));
});
