'use strict';
const test=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs/promises'),path=require('node:path'),os=require('node:os');
const {createHash}=require('node:crypto');
const {dumpResource,dumpSdFile}=require('./dump.cjs');
for(const failure of [false,true])test('dump '+(failure?'retains marked partial output without retries':'writes exact range and checksum without overwriting'),async()=>{
 const dir=await fs.mkdtemp(path.join(os.tmpdir(),'hcb-bulk-'));const dest=path.join(dir,'capture.bin');
 const source=Buffer.from('0123456789abcdef');let reads=0;
 const bridge={readInfo:async()=>({size:16n,kind:'file',maxChunk:4}),readBulk:async(id,offset,size)=>{reads++;if(failure&&reads===2)throw Error('read failed');return source.subarray(Number(offset),Number(offset)+size);}};
 try{
  if(failure){await assert.rejects(dumpResource(bridge,1,dest),/read failed/);assert.equal(reads,2);assert.equal((await fs.readFile(dest)).toString(),'0123');const meta=JSON.parse(await fs.readFile(dest+'.json'));assert.equal(meta.complete,false);assert.equal(meta.bytesWritten,'4');}
  else{const result=await dumpResource(bridge,1,dest,{offset:2,length:10});assert.deepEqual(await fs.readFile(dest),source.subarray(2,12));assert.equal(result.sha256,createHash('sha256').update(source.subarray(2,12)).digest('hex'));assert.equal(result.complete,true);await assert.rejects(dumpResource(bridge,1,dest),/EEXIST/);}
 }finally{await fs.rm(dir,{recursive:true,force:true});}
});
test('SD helper only opens read mode and closes resources in reverse order',async()=>{
 const dir=await fs.mkdtemp(path.join(os.tmpdir(),'hcb-sd-'));const requests=[],closed=[];
 const bridge={henResource:async()=>1,domain:async id=>id,closeResource:async id=>closed.push(id),ipc:async(id,args)=>{requests.push(args);return{result:0,objects:[id+1]};},readInfo:async()=>({size:0n,kind:'file',maxChunk:8})};
 try{await dumpSdFile(bridge,'/switch/test.nro',path.join(dir,'out'));assert.deepEqual(requests.map(r=>r.commandId),[18,8]);assert.equal(requests[1].data.readUInt32LE(),1);assert.deepEqual(closed,[3,2,1]);await assert.rejects(dumpSdFile(bridge,'/../bad','unused'),/traversal/);}
 finally{await fs.rm(dir,{recursive:true,force:true});}
});
