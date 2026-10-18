// Browser QA fixture only. Never served by tools/server.py or included in a release.
const sample={id:'00000000-0000-4000-8000-000000000001',issuer:'Example',account:'test@example.com',secret:'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ',algorithm:'SHA1',digits:6,period:30};
let records=[sample], staged=[], exported=[], authorized=true, importCount=0;
const port={
  async open(){
    authorized=true;
    this.readable=new ReadableStream({start:c=>{this.input=c;}});
    this.writable=new WritableStream({write:bytes=>{
      const req=JSON.parse(new TextDecoder().decode(bytes));let result={};
      const args=req.args;
      switch(req.op){
        case 'status':result={firmware:'TEST-FIXTURE',batchImport:true,state:'unlocked',authorized,timeValid:true,time:Math.floor(Date.now()/1000),batteryMv:3900};break;
        case 'list':result={total:records.length,records:records.slice(args.offset,args.offset+5).map(({secret,...r})=>r)};break;
        case 'upsert':records.push(args.record);break;
        case 'import_begin':staged=[];importCount=args.count;break;
        case 'import_item':staged.push(args.record);break;
        case 'import_commit':
          if(staged.length!==importCount)throw new Error('Incomplete test batch');
          for(const record of staged){const index=records.findIndex(r=>r.id===record.id);if(index<0)records.push(record);else records[index]=record;}
          staged=[];importCount=0;break;
        case 'import_abort':staged=[];importCount=0;break;
        case 'rename':Object.assign(records.find(r=>r.id===args.id),{issuer:args.issuer,account:args.account});break;
        case 'delete':records=records.filter(r=>r.id!==args.id);break;
        case 'export_begin':exported=records.slice();result={count:exported.length};break;
        case 'export_next':result={record:exported.shift()};break;
        case 'restore_begin':staged=[];break;
        case 'restore_item':staged.push(args.record);break;
        case 'restore_commit':records=staged;staged=[];break;
        case 'restore_abort':staged=[];break;
        case 'lock':authorized=false;break;
      }
      this.input.enqueue(new TextEncoder().encode(JSON.stringify({v:1,id:req.id,ok:true,result})+'\n'));
    }});
  },
  async setSignals(){},async close(){}
};
Object.defineProperty(navigator,'serial',{value:{requestPort:async()=>port},configurable:true});
document.addEventListener('DOMContentLoaded',()=>{
  const banner=document.createElement('div');banner.textContent='ТЕСТОВЫЙ СТЕНД — USB И УСТРОЙСТВО ИМИТИРУЮТСЯ';banner.className='notice';document.body.prepend(banner);
});
