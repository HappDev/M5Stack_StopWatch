import test from 'node:test';
import assert from 'node:assert/strict';
import {parseGoogleMigration,MigrationBatch,selectMigrationRecords} from '../web/migration.mjs';
import {entry,migration,uri,blob,number} from './fixtures/migration.mjs';

test('Google export preserves RFC key and TOTP parameters',()=>{
  const part=parseGoogleMigration(migration());const r=part.records[0];
  assert.equal(r.secret,'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ');assert.equal(r.issuer,'Example');assert.equal(r.account,'alice@example.com');
  assert.equal(r.algorithm,'SHA1');assert.equal(r.digits,6);assert.equal(r.period,30);
  assert.equal(part.batchIndex,0);assert.equal(part.batchSize,1);assert.equal(part.batchId,42);
  const variants=parseGoogleMigration(migration([entry({algorithm:2,digits:2,name:'Почта:я',issuer:'Почта'}),entry({algorithm:3,issuer:'',name:'Other:bob'})]));
  assert.equal(variants.records[0].account,'я');assert.equal(variants.records[0].algorithm,'SHA256');assert.equal(variants.records[0].digits,8);
  assert.equal(variants.records[1].issuer,'Other');assert.equal(variants.records[1].account,'bob');assert.equal(variants.records[1].algorithm,'SHA512');
});

test('int32 batch IDs include negative and omitted zero index',()=>{
  assert.equal(parseGoogleMigration(migration(undefined,{id:-2147483648})).batchId,-2147483648);
  assert.equal(parseGoogleMigration(uri([...blob(1,entry()),...number(2,1)])).batchIndex,0);
  assert.throws(()=>parseGoogleMigration(migration(undefined,{id:2n**40n})));
});

test('base64 handles escaped plus and unescaped plus without changing bytes',()=>{
  const source=migration([entry({secret:new Uint8Array(20).fill(255)})]);
  assert.ok(source.includes('%2F') || source.includes('%2B'));
  assert.equal(parseGoogleMigration(source).records[0].secret,parseGoogleMigration(decodeURIComponent(source)).records[0].secret);
});

test('malformed, duplicate, truncated and oversized protobuf never produces records',()=>{
  const valid=migration();
  for(const bad of ['',valid.replace('offline','other'),valid+'&data=x',valid+'#x',valid.replace('data=','other='),
    'otpauth-migration://offline?data=!', 'otpauth-migration://offline?data=A',
    uri([0]),uri([10,99,1]),uri([0x80]),uri(new Uint8Array(11).fill(0x80)),
    uri([...blob(1,entry()),...number(2,1),...number(2,1)]),
    uri([...blob(1,entry()),...blob(2,'wrong wire type')]),
    migration(undefined,{size:0}),migration(undefined,{size:101}),migration(undefined,{index:1}),
    migration(undefined,{version:2}),migration([]), 'x'.repeat(65537)])assert.throws(()=>parseGoogleMigration(bad));
});

test('unsupported entries reject the whole QR before any import',()=>{
  for(const overrides of [{type:1},{type:0},{algorithm:0},{algorithm:4},{digits:0},{digits:3},{secret:'short'},{issuer:'Я'.repeat(49)},
    {name:'a\nb'},{secret:'x'.repeat(129)},{name:new Uint8Array([255])}]){
    assert.throws(()=>parseGoogleMigration(migration([entry(),entry(overrides)])));
  }
  assert.throws(()=>parseGoogleMigration(migration(Array.from({length:101},()=>entry()))),/100/);
});

test('unknown protobuf fields can be skipped, known duplicates are rejected',()=>{
  const withUnknown=[...entry(),...number(100,123),...blob(101,'future')];
  assert.equal(parseGoogleMigration(migration([withUnknown])).records.length,1);
  assert.throws(()=>parseGoogleMigration(migration([[...entry(),...number(4,2)]])));
});

test('multi-QR batches are complete only after every part; repeated QR is harmless',()=>{
  const batch=new MigrationBatch();const a=migration([entry()],{size:2,index:0,id:9});
  const b=migration([entry({name:'Example:bob'})],{size:2,index:1,id:9});
  assert.equal(batch.add(b),true);assert.equal(batch.complete,false);
  assert.throws(()=>selectMigrationRecords(batch,new Set(batch.records.map(r=>r.id)),[]),/все QR/);
  assert.equal(batch.add(b),false);assert.equal(batch.records.length,1);
  batch.add(a);assert.equal(batch.complete,true);assert.equal(batch.records[0].account,'alice@example.com');
  const id=batch.records[0].id;batch.add(a);assert.equal(batch.records[0].id,id);
  assert.throws(()=>batch.add(migration()),/другого экспорта/);
  assert.throws(()=>batch.add(migration([entry({name:'changed'})],{size:2,index:0,id:9})),/разные аккаунты/);
  assert.equal(batch.records.length,2);
});

test('selection validates capacity and duplicates without replacing existing records',()=>{
  const batch=new MigrationBatch();batch.add(migration([entry(),entry({name:'Example:bob'})]));
  const [a,b]=batch.records;const selected=new Set([b.id]);
  assert.deepEqual(selectMigrationRecords(batch,selected,[a]),[b]);
  assert.throws(()=>selectMigrationRecords(batch,new Set([a.id]),[a]),/уже есть/);
  assert.throws(()=>selectMigrationRecords(batch,new Set(),[]),/Выберите/);
  const existing=Array.from({length:100},(_,i)=>({issuer:'Existing',account:String(i)}));
  assert.throws(()=>selectMigrationRecords(batch,selected,existing),/места/);
  assert.equal(selectMigrationRecords(batch,selected,existing.slice(0,99)).length,1);
  const duplicated=new MigrationBatch();duplicated.add(migration([entry(),entry()]));
  assert.throws(()=>selectMigrationRecords(duplicated,new Set(duplicated.records.map(r=>r.id)),[]),/уже есть/);
});

test('batch total is bounded and cleanup removes retained secrets',()=>{
  const batch=new MigrationBatch();batch.add(migration(Array.from({length:100},()=>entry()),{size:2}));
  assert.throws(()=>batch.add(migration([entry()],{size:2,index:1})),/100/);
  const retained=batch.records;batch.clear();assert.equal(batch.records.length,0);assert.equal(batch.complete,false);
  assert.ok(retained.every(r=>r.secret===''));assert.equal(batch.metadata,null);
});
