'use strict';
const fs=require('node:fs/promises');
const path=require('node:path');
const {createHash}=require('node:crypto');
const {performance}=require('node:perf_hooks');
async function dumpResource(bridge,resource,destination,options={}) {
  const info=await bridge.readInfo(resource);
  const offset=BigInt(options.offset??0),length=options.length===undefined?info.size-offset:BigInt(options.length);
  const chunk=options.chunk??info.maxChunk;
  if(offset<0n || length<0n || offset>info.size || length>info.size-offset) throw Error('Dump range exceeds source');
  if(!Number.isInteger(chunk)||chunk<1||chunk>info.maxChunk)throw Error('Invalid dump block size');
  const filename=path.resolve(destination),recordPath=filename+'.json';
  // Exclusive creation: existing files are never overwritten. A partial dump
  // remains with complete=false; read failures are never automatically retried.
  const recordFile=await fs.open(recordPath,'wx');
  let output;
  const report={createdUtc:new Date().toISOString(),sourceKind:info.kind,sourceSize:info.size.toString(),offset:offset.toString(),length:length.toString(),chunk,bytesWritten:'0',complete:false};
  const persist=async(durable=false)=>{
    const bytes=Buffer.from(JSON.stringify(report,null,2)+'\n');await recordFile.truncate(0);let at=0;
    while(at<bytes.length){const result=await recordFile.write(bytes,at,bytes.length-at,at);if(!result.bytesWritten)throw Error('Metadata write made no progress');at+=result.bytesWritten;}
    if(durable)await recordFile.sync();
  };
  const digest=createHash('sha256');let written=0n;const start=performance.now();let lastRecord=start;
  try {
    output=await fs.open(filename,'wx');
    await persist(true);
    while(written<length){
      const count=Number(length-written<BigInt(chunk)?length-written:BigInt(chunk));
      const bytes=await bridge.readBulk(resource,offset+written,count);
      if(!bytes.length||bytes.length>count)throw Error('Unexpected end or oversized source read');
      let at=0;
      while(at<bytes.length){const result=await output.write(bytes,at,bytes.length-at,null);if(!result.bytesWritten)throw Error('PC file write made no progress');digest.update(bytes.subarray(at,at+result.bytesWritten));at+=result.bytesWritten;written+=BigInt(result.bytesWritten);}
      report.bytesWritten=written.toString();
      if(performance.now()-lastRecord>=1000){await persist();lastRecord=performance.now();}
      if(options.onProgress)await options.onProgress({bytes:written,total:length});
    }
    await output.sync();
    const after=await bridge.readInfo(resource);
    if(after.size!==info.size||after.kind!==info.kind)throw Error('Source size/interface changed during dump');
    report.sha256=digest.digest('hex');report.complete=true;
    report.milliseconds=performance.now()-start;
    report.MiBPerSecond=Number(written)/1048576*1000/Math.max(report.milliseconds,0.001);
    report.consistency='Sequential read; unchanged size does not establish an atomic snapshot of mutable data';
    await persist(true);
  }catch(error){report.error=error.message;report.bytesWritten=written.toString();report.complete=false;await persist(true);throw error;}
  finally {if(output)await output.close();await recordFile.close();}
  return {...report,file:filename,record:recordPath};
}
async function dumpSdFile(bridge,remote,destination,options={}) {
  const bytes=Buffer.from(remote);
  if(!remote.startsWith('/')||bytes.length>=769||bytes.includes(0)||remote.includes('\\')||remote.split('/').some(c=>c==='.'||c==='..'))throw Error('Use an absolute SD path without traversal');
  const pathBytes=Buffer.alloc(769);bytes.copy(pathBytes);
  let root,sd,file;
  const checked=async(resource,args)=>{const result=await bridge.ipc(resource,args);if(result.result)throw Error('Read-only file open returned 0x'+result.result.toString(16));return result;};
  try{
    root=await bridge.henResource('fs');root=await bridge.domain(root);
    sd=(await checked(root,{commandId:18,outObjects:1})).objects[0];
    const mode=Buffer.alloc(4);mode.writeUInt32LE(1);
    file=(await checked(sd,{commandId:8,data:mode,outObjects:1,buffers:[{attr:'in|pointer',data:pathBytes}]})).objects[0];
    return await dumpResource(bridge,file,destination,options);
  }finally {if(file)await bridge.closeResource(file);if(sd)await bridge.closeResource(sd);if(root)await bridge.closeResource(root);}
}
module.exports={dumpResource,dumpSdFile};
