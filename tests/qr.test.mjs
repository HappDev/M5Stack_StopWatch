import test from 'node:test';
import assert from 'node:assert/strict';
import {scanQrCamera} from '../web/qr.mjs';

const tick=()=>new Promise(resolve=>setImmediate(resolve));
function cameraMock(getUserMedia){
  const original=Object.getOwnPropertyDescriptor(globalThis,'navigator');
  Object.defineProperty(globalThis,'navigator',{configurable:true,value:{mediaDevices:{getUserMedia}}});
  return ()=>{if(original)Object.defineProperty(globalThis,'navigator',original);else delete globalThis.navigator;};
}
function video(){return {srcObject:null,readyState:0,pause(){this.paused=true;},async play(){this.paused=false;}};}

test('closing camera before permission resolves stops late tracks without starting video',async()=>{
  let grant,stopped=0,errors=0;
  const restore=cameraMock(()=>new Promise(resolve=>{grant=resolve;}));
  try{
    const target=video(),stop=scanQrCamera(target,()=>assert.fail('No QR expected'),()=>errors++);
    stop();grant({getTracks:()=>[{stop:()=>stopped++}]});await tick();
    assert.equal(stopped,1);assert.equal(target.srcObject,null);assert.equal(errors,0);
  }finally{restore();}
});

test('closing active camera clears stream and stops scanning',async()=>{
  let stopped=0;
  const stream={getTracks:()=>[{stop:()=>stopped++}]};
  const restore=cameraMock(async()=>stream);
  try{
    const target=video(),stop=scanQrCamera(target,()=>assert.fail('No QR expected'),()=>assert.fail('No error expected'));
    await tick();assert.equal(target.srcObject,stream);stop();
    assert.equal(stopped,1);assert.equal(target.srcObject,null);assert.equal(target.paused,true);
  }finally{restore();}
});

test('camera permission failure is reported and playback failure releases tracks',async()=>{
  let error;
  let restore=cameraMock(async()=>{throw new DOMException('Denied','NotAllowedError');});
  try{scanQrCamera(video(),()=>{},e=>{error=e;});await tick();assert.match(error.message,/Разрешите камеру/);}
  finally{restore();}
  let stopped=0;error=null;
  restore=cameraMock(async()=>({getTracks:()=>[{stop:()=>stopped++}]}));
  try{
    scanQrCamera({...video(),async play(){throw new Error('Playback failed');}},()=>{},e=>{error=e;});
    await tick();assert.equal(stopped,1);assert.equal(error.message,'Playback failed');
  }finally{restore();}
});
