import test from 'node:test';
import assert from 'node:assert/strict';
import {importBatch} from '../web/import.mjs';
import {normalizeRecord} from '../web/otp.mjs';

const record=()=>normalizeRecord({issuer:'Public test',account:'alice',secret:'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ'});
test('batch import sends all records then exactly one commit, with no export or per-account writes',async()=>{
  const input=Array.from({length:100},record),calls=[],progress=[],references=[];
  const device={async request(op,args){calls.push({op,args:structuredClone(args)});if(args?.record)references.push(args.record);return {};}};
  await importBatch(device,{batchImport:true},input,p=>progress.push(p));
  assert.deepEqual(calls.map(c=>c.op),['import_begin',...Array(100).fill('import_item'),'import_commit']);
  assert.equal(calls[0].args.count,100);
  for(let i=0;i<100;i++){assert.equal(calls[i+1].args.index,i);assert.deepEqual(calls[i+1].args.record,input[i]);}
  assert.deepEqual(progress.at(-1),{phase:'confirming',total:100});
  assert.ok(references.every(r=>r.secret===''));assert.ok(input.every(r=>r.secret.length));
});
test('old firmware and invalid batches are rejected before any USB mutation',async()=>{
  const device={async request(){assert.fail('No request expected');}};
  for(const status of [{},{batchImport:false},{batchImport:'true'}])await assert.rejects(importBatch(device,status,[record()]),/обновите прошивку/);
  for(const rows of [[],[{issuer:'Bad',secret:'bad'}],Array.from({length:101},record)])await assert.rejects(importBatch(device,{batchImport:true},rows));
});
test('item failure aborts staging and never commits or retries a write',async()=>{
  const calls=[],input=[record(),record(),record()];
  const device={async request(op,args){calls.push(op);if(op==='import_item'&&args.index===1)throw new Error('USB disconnected');return {};}};
  await assert.rejects(importBatch(device,{batchImport:true},input),/до сохранения/);
  assert.deepEqual(calls,['import_begin','import_item','import_item','import_abort']);
});
test('lost commit reply reports uncertain result without sending the batch twice',async()=>{
  const calls=[];
  const device={async request(op){calls.push(op);if(op==='import_commit'||op==='import_abort')throw new Error('USB disconnected');return {};}};
  await assert.rejects(importBatch(device,{batchImport:true},[record()]),/Проверьте список/);
  assert.deepEqual(calls,['import_begin','import_item','import_commit','import_abort']);
});
