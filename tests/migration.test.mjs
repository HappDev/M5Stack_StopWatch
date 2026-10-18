import test from 'node:test';
import assert from 'node:assert/strict';
import {parseGoogleMigration,MigrationBatch,selectMigrationRecords} from '../web/migration.mjs';
import {entry,migration,uri,blob,number} from './fixtures/migration.mjs';
import {version2,withoutMetadata} from './fixtures/migration-public.mjs';

test('published version 2 export decodes all three accounts without our fixture encoder',()=>{
  const part=parseGoogleMigration(version2);
  assert.equal(part.version,2);assert.equal(part.batchSize,1);assert.equal(part.batchIndex,0);
  assert.deepEqual(part.records.map(({issuer,account,secret,algorithm,digits,period})=>({issuer,account,secret,algorithm,digits,period})),[
    {issuer:'Test1',account:'test1@example1.com',secret:'JBSWY3DPEHPK3PXP',algorithm:'SHA1',digits:6,period:30},
    {issuer:'Test2',account:'test2@example2.com',secret:'JBSWY3DPEHPK3PXQ',algorithm:'SHA1',digits:6,period:30},
    {issuer:'Test3',account:'test3@example3.com',secret:'JBSWY3DPEHPK3PXR',algorithm:'SHA1',digits:6,period:30}
  ]);
});

test('published export with omitted metadata uses single QR and SHA1/6 defaults',()=>{
  const batch=new MigrationBatch();batch.add(withoutMetadata);
  assert.equal(batch.complete,true);assert.equal(batch.metadata.version,0);
  const [{id,...record}]=batch.records;
  assert.deepEqual(record,{issuer:'Example',account:'alice@google.com',secret:'JBSWY3DPEHPK3PXP',algorithm:'SHA1',digits:6,period:30});
});

test('protobuf unspecified OTP enums use TOTP/SHA1/6 and preserve explicit parameters',()=>{
  for(const version of [0,1,2]){
    const part=parseGoogleMigration(migration([entry({type:0,algorithm:0,digits:0})],{version}));
    assert.equal(part.records[0].algorithm,'SHA1');assert.equal(part.records[0].digits,6);
    const omitted=uri([...blob(1,[...blob(1,'12345678901234567890'),...blob(2,'Test:user')]),...number(2,version)]);
    assert.equal(parseGoogleMigration(omitted).records[0].period,30);
  }
  assert.throws(()=>parseGoogleMigration(migration([[...entry({type:0}),...number(7,5)]])),/счётчик/);
});

test('future export versions identify only the version, never the URI or secret',()=>{
  for(const version of [-1,3,2147483647]){
    assert.throws(()=>parseGoogleMigration(migration(undefined,{version})),{message:`Версия экспорта Google Authenticator ${version} пока не поддерживается (поддерживаются 0, 1 и 2). Сообщите только этот номер версии.`});
  }
});

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
    migration(undefined,{version:3}),migration([]), 'x'.repeat(65537)])assert.throws(()=>parseGoogleMigration(bad));
});

test('unsupported entries reject the whole QR before any import',()=>{
  for(const overrides of [{type:1},{type:3},{algorithm:4},{algorithm:5},{digits:3},{secret:'short'},{issuer:'Я'.repeat(49)},
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

test('version 2 batches preserve ordering and reject mixing versions',()=>{
  const batch=new MigrationBatch();
  const first=migration([entry()],{version:2,size:2,index:0,id:17});
  const second=migration([entry({name:'Example:bob'})],{version:2,size:2,index:1,id:17});
  batch.add(second);assert.equal(batch.complete,false);
  assert.throws(()=>batch.add(migration([entry()],{version:1,size:2,index:0,id:17})),/другого экспорта/);
  batch.add(first);assert.equal(batch.complete,true);assert.equal(batch.add(first),false);
  assert.deepEqual(batch.records.map(r=>r.account),['alice@example.com','bob']);
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
