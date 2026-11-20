'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const net = require('node:net');
const {connect, MAGIC, VERSION, operations, words, encodeIpc, decodeIpc} = require('./client.cjs');

async function mockBridge(run) {
  const sockets = new Set();
  const received = [];
  const server = net.createServer(socket => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    socket.on('error', () => {});
    let bytes = Buffer.alloc(0);
    socket.on('data', chunk => {
      bytes = Buffer.concat([bytes, chunk]);
      while (bytes.length >= 24) {
        const length = bytes.readUInt32LE(12);
        if (bytes.length < 24 + length) return;
        const header = Buffer.from(bytes.subarray(0, 24));
        const payload = Buffer.from(bytes.subarray(24, 24 + length));
        bytes = bytes.subarray(24 + length);
        assert.equal(header.readUInt32LE(), MAGIC);
        assert.equal(header.readUInt16LE(4), VERSION);
        const operation = header.readUInt16LE(6);
        received.push({operation, payload});
        const reply = (body = Buffer.alloc(0), result = 0, corrupt = false, version = VERSION) => {
          const response = Buffer.from(header);
          response.writeUInt16LE(version, 4);
          response.writeUInt32LE(body.length, 12);
          response.writeUInt32LE(result, 16);
          if (corrupt) response.writeUInt32LE(header.readUInt32LE(8) + 1, 8);
          // Deliberately split the reply within its fixed-size header.
          socket.write(response.subarray(0, 7));
          setImmediate(() => { if (!socket.destroyed) socket.write(Buffer.concat([response.subarray(7), body])); });
        };
        run({operation, payload, reply, socket});
      }
    });
  });
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
  return {port: server.address().port, received, close: async () => {
    for (const socket of sockets) socket.destroy();
    await new Promise(resolve => server.close(resolve));
  }};
}

test('connects directly and serializes concurrent commands across fragmented replies', async () => {
  const mock = await mockBridge(({operation, payload, reply}) => {
    assert.equal(payload.length, 0);
    reply(Buffer.from(JSON.stringify({operation})));
  });
  let bridge;
  try {
    bridge = await connect({host: '127.0.0.1', port: mock.port});
    const values = await Promise.all(Array.from({length: 20}, () => bridge.status()));
    assert(values.every(value => value.operation === operations.status));
    assert.equal(mock.received.length, 20);
    assert.equal(mock.received[0].operation, operations.status);
  } finally { if (bridge) await bridge.close(); await mock.close(); }
});

test('rejects replies using the older token-based wire protocol', async () => {
  const mock = await mockBridge(({reply}) => reply(Buffer.from('{}'), 0, false, 1));
  let bridge;
  try {
    bridge = await connect({host: '127.0.0.1', port: mock.port});
    await assert.rejects(bridge.status(), /Invalid bridge reply header/);
    assert.deepEqual(mock.received.map(item => item.operation), [operations.status]);
  } finally { if (bridge) await bridge.close(); await mock.close(); }
});

test('timeout closes the session and never replays a state-changing command', async () => {
  const mock = await mockBridge(() => {});
  let bridge;
  try {
    bridge = await connect({host: '127.0.0.1', port: mock.port, timeout: 100});
    await assert.rejects(bridge.writeAllocation(1, 0, Buffer.from('one attempt')), /Outcome unknown/);
    await assert.rejects(bridge.status(), /closed/);
    assert.deepEqual(mock.received.map(item => item.operation), [operations.write]);
  } finally { if (bridge) await bridge.close(); await mock.close(); }
});

test('rejects mismatched request IDs and does not run queued work afterward', async () => {
  const mock = await mockBridge(({reply}) => reply(Buffer.from('{}'), 0, true));
  let bridge;
  try {
    bridge = await connect({host: '127.0.0.1', port: mock.port});
    const first = bridge.status();
    const second = bridge.status();
    await assert.rejects(first, /Invalid bridge reply header/);
    await assert.rejects(second, /closed/);
    assert.equal(mock.received.length, 1);
  } finally { if (bridge) await bridge.close(); await mock.close(); }
});

test('IPC preserves returned objects, handles, nonzero Results and output buffers', () => {
  const object = Buffer.alloc(32); object.writeUInt32LE(7); object.writeUInt32LE(1, 4);
  const handle = Buffer.alloc(32); handle.writeUInt32LE(8); handle.writeUInt32LE(2, 4);
  const reply = Buffer.concat([words(0, 4, 1, 1, 1), object, handle, words(3), Buffer.from('data'), Buffer.from('out')]);
  const parsed = decodeIpc(reply);
  assert.equal(parsed.objects[0].id, 7);
  assert.equal(parsed.handles[0].kind, 'handle');
  assert.equal(parsed.data.toString(), 'data');
  assert.equal(parsed.buffers[0].toString(), 'out');
  assert.equal(decodeIpc(words(0x1234, 0, 0, 0, 0)).result, 0x1234);
  assert.throws(() => decodeIpc(reply.subarray(0, reply.length - 1)), /Truncated/);
  assert.throws(() => decodeIpc(Buffer.concat([reply, Buffer.alloc(1)])), /Trailing/);
});

test('IPC encodes copied input handles and domain object IDs without numeric precision loss', () => {
  const bytes = encodeIpc({id: 1}, {commandId: 42, sendPid: true, data: words(0x12345678), outSize: 16,
    handles: [{id: 2}], objects: [{id: 3}], outHandles: ['copy'], outObjects: 1,
    buffers: [{attr: 'out|pointer|fixed', size: 16}]});
  assert.equal(bytes.length, 88);
  assert.equal(bytes.readUInt32LE(12), 1);
  assert.equal(bytes.readUInt32LE(48), 26);
  assert.equal(bytes.readUInt32LE(56), 2);
  assert.equal(bytes.readUInt32LE(60), 1);
  assert.equal(bytes.readUInt32LE(64), 3);
  assert.equal(bytes.readUInt32LE(68), 0x12345678);
  assert.throws(() => encodeIpc(1, {commandId: 0, outSize: 129}), /128/);
  assert.throws(() => encodeIpc(1, {commandId: 0, buffers: [{attr: 'out|pointer', size: 65536}]}), /Invalid/);
  assert.throws(() => encodeIpc({id: 1, objectId: 1}, {commandId: 0, data: Buffer.alloc(1), objects: [2]}), /aligned/);
  assert.throws(() => encodeIpc({id: 1, objectId: 1}, {commandId: 0, outSize: 3, outObjects: 1}), /aligned/);
});

test('64-bit memory addresses retain their full value on the wire', async () => {
  const address = 0x123456789abcdef0n;
  const mock = await mockBridge(({operation, payload, reply}) => {
    assert.equal(operation, operations.read);
    assert.equal(payload.readBigUInt64LE(), address);
    reply(Buffer.from('abcd'));
  });
  let bridge;
  try {
    bridge = await connect({host: '127.0.0.1', port: mock.port});
    assert.equal((await bridge.readMemory(address, 4)).toString(), 'abcd');
    await assert.rejects(bridge.readMemory(Number(address), 4), /BigInt/);
    assert.equal(mock.received.length, 1);
  } finally { if (bridge) await bridge.close(); await mock.close(); }
});

test('eight-MiB reply is reconstructed correctly and EOF/errors preserve session alignment', async () => {
  const body=Buffer.alloc(8*1024*1024,0xa7);
  const mock=await mockBridge(({operation,payload,reply})=>{
    if(operation===operations.networkRead)reply(body);
    else if(operation===operations.readBulk)reply();
    else if(operation===operations.readInfo)reply(Buffer.alloc(0),0xf0010009);
    else reply(Buffer.from('{}'));
  });
  let bridge;
  try{
    bridge=await connect({host:'127.0.0.1',port:mock.port});
    assert.deepEqual(await bridge.networkRead(body.length),body);
    assert.equal((await bridge.readBulk(1,0n,1)).length,0);
    await assert.rejects(bridge.readInfo(1),/0xf0010009/);
    assert.deepEqual(await bridge.status(),{});
  }finally{if(bridge)await bridge.close();await mock.close();}
});

test('large ordinary reply and short bulk reply are rejected', async () => {
  for(const bulk of [false,true]){
    const mock=await mockBridge(({reply})=>reply(Buffer.alloc(bulk?1:1024*1024+1)));
    let bridge;
    try{
      bridge=await connect({host:'127.0.0.1',port:mock.port});
      await assert.rejects(bulk?bridge.networkRead(2):bridge.status(),bulk?/Short network/:/Invalid bridge reply header/);
    }finally{if(bridge)await bridge.close();await mock.close();}
  }
});
