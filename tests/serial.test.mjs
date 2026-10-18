import test from 'node:test';
import assert from 'node:assert/strict';
import {Device,FrameReader,MAX_FRAME} from '../web/serial.mjs';
const encoder=new TextEncoder();
test('fragmented UTF-8 frames and multiple messages in a USB read',()=>{
  const frames=[],errors=[];const parser=new FrameReader(x=>frames.push(x),e=>errors.push(e));
  const data=encoder.encode('{"v":1,"text":"Почта"}\n{"id":2}\r\n');
  for(let i=0;i<data.length;i++)parser.push(data.slice(i,i+1));
  assert.deepEqual(frames,[{v:1,text:'Почта'},{id:2}]);assert.equal(errors.length,0);
});
test('oversized and malformed frames cannot desynchronize following frames',()=>{
  const frames=[],errors=[];const parser=new FrameReader(x=>frames.push(x),e=>errors.push(e));
  parser.push(encoder.encode('a'.repeat(MAX_FRAME+10)+'\n{"id":3}\n{bad}\n{"id":4}\n'));
  assert.deepEqual(frames,[{id:3},{id:4}]);assert.equal(errors.length,2);
});
test('clear drops a partial message on disconnect',()=>{
  const frames=[];const parser=new FrameReader(x=>frames.push(x));parser.push(encoder.encode('{"secret":"partial'));
  parser.clear();parser.push(encoder.encode('{"id":5}\n'));assert.deepEqual(frames,[{id:5}]);
});
test('Web Serial session handshake, queued requests, rejection and disconnect',async()=>{
  const calls=[];let input;let signals=[];let closed=false;
  const port={
    async open(){},async close(){closed=true;},async setSignals(s){signals.push(s);},
    readable:new ReadableStream({start(controller){input=controller;}}),
    writable:new WritableStream({write(bytes){
      const request=JSON.parse(new TextDecoder().decode(bytes));calls.push(request.op);
      const response=request.op==='fail'?{v:1,id:request.id,ok:false,error:{message:'Rejected'}}:
        {v:1,id:request.id,ok:true,result:{state:'unlocked',authorized:true,op:request.op}};
      const frame=encoder.encode(JSON.stringify(response)+'\n');
      input.enqueue(frame.slice(0,7));input.enqueue(frame.slice(7));
    }})
  };
  Object.defineProperty(navigator,'serial',{value:{requestPort:async()=>port},configurable:true});
  let disconnects=0;const d=new Device(()=>disconnects++);
  assert.equal((await d.connect()).state,'unlocked');
  const [a,b]=await Promise.all([d.request('list'),d.request('time_set',{epoch:1234567890})]);
  assert.equal(a.op,'list');assert.equal(b.op,'time_set');
  await assert.rejects(d.request('fail'),/Rejected/);
  await d.disconnect();assert.equal(d.connected,false);assert.equal(disconnects,1);assert(closed);
  assert.equal(signals[0].dataTerminalReady,true);assert.equal(signals.at(-1).dataTerminalReady,false);
  assert.deepEqual(calls,['status','list','time_set','fail']);
  await assert.rejects(d.request('list'));
  delete navigator.serial;
});
