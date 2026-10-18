import test from 'node:test';
import assert from 'node:assert/strict';
import {decodeBase32,encodeBase32,normalizeRecord,parseOtpAuth,validateRecords} from '../web/otp.mjs';

const secret='GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ';
test('RFC 4648 Base32 vectors and canonical padding',()=>{
  const vectors=[['f','MY======'],['fo','MZXQ===='],['foo','MZXW6==='],['foob','MZXW6YQ='],['fooba','MZXW6YTB'],['foobar','MZXW6YTBOI======']];
  for(const [plain,b32] of vectors){assert.equal(new TextDecoder().decode(decodeBase32(b32)),plain);assert.equal(encodeBase32(new TextEncoder().encode(plain)),b32.replace(/=/g,''));}
  for(const invalid of ['A','AAA','AAAAAA','MZ','MY=','MY======A','M1','!','MY======='])assert.throws(()=>decodeBase32(invalid),invalid);
});
test('URI defaults, URL decoding, explicit parameters',()=>{
  const r=parseOtpAuth(`otpauth://totp/Example%3Aalice%40example.com?secret=${secret}&issuer=Example`);
  assert.equal(r.issuer,'Example');assert.equal(r.account,'alice@example.com');assert.equal(r.algorithm,'SHA1');assert.equal(r.digits,6);assert.equal(r.period,30);
  const custom=parseOtpAuth(`otpauth://totp/%D0%9F%D0%BE%D1%87%D1%82%D0%B0%3Auser?secret=${secret}&algorithm=SHA512&digits=8&period=60`);
  assert.equal(custom.issuer,'Почта');assert.equal(custom.algorithm,'SHA512');assert.equal(custom.period,60);
});
test('reject unsafe or unsupported URI parameters',()=>{
  for(const uri of [
    `otpauth://hotp/test?secret=${secret}`,`https://totp/test?secret=${secret}`,
    `otpauth://totp/A%3Auser?issuer=B&secret=${secret}`,
    `otpauth://totp/test?secret=${secret}&secret=${secret}`,
    `otpauth://totp/test?secret=${secret}&algorithm=MD5`,
    `otpauth://totp/test?secret=${secret}&digits=7`,
    `otpauth://totp/test?secret=${secret}&period=15`,
    `otpauth://totp/test?secret=${secret}&counter=1`,
    `otpauth://totp/test?secret=${secret}#fragment`,
    `otpauth://totp/%00test?secret=${secret}`,
    `otpauth://totp/%ZZ?secret=${secret}`
  ])assert.throws(()=>parseOtpAuth(uri),uri);
});
test('byte limits, secret limits and record count apply before USB',()=>{
  const base={issuer:'Mail',account:'alice',secret};
  assert.equal(normalizeRecord({...base,issuer:'Я'.repeat(48)}).issuer.length,48);
  assert.throws(()=>normalizeRecord({...base,issuer:'Я'.repeat(49)}));
  assert.throws(()=>normalizeRecord({...base,account:'a\nb'}));
  assert.throws(()=>normalizeRecord({...base,secret:'MY'}));
  assert.throws(()=>normalizeRecord({...base,id:'not-a-uuid'}));
  const r=normalizeRecord(base);assert.throws(()=>validateRecords([r,r]));
  assert.throws(()=>validateRecords(Array.from({length:101},()=>base)));
});
