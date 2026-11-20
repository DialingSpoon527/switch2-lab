'use strict';
const net = require('node:net');

const MAGIC = 0x31424348;
const VERSION = 2;
const MAX_PACKET = 1024 * 1024;
const MAX_BUFFER = 256 * 1024;
const operations = Object.freeze({
  status: 2, getInfo: 3, query: 4, read: 5, alloc: 6, write: 7,
  close: 8, resources: 9, open: 10, clone: 11, domain: 12, ipc: 13,
  asService: 14, hen: 15, jit: 16, call: 17, symbols: 18, hints: 19,
});
const errors = {
  0xf0010001: 'invalid command or payload',
  0xf0010003: 'resource unavailable in this connection', 0xf0010004: 'bridge limit exceeded',
  0xf0010005: 'close child objects first', 0xf0010006: 'invalid memory range',
  0xf0010007: 'unexpected IPC reply layout', 0xf0010008: 'shared FS identity must be preserved',
};
const bufferAttributes = Object.freeze({
  in: 1, out: 2, map: 4, pointer: 8, fixed: 16, auto: 32, nonsecure: 64, nondevice: 128,
});

function u32(value) {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) throw Error('Expected a uint32');
  return value;
}
function u64(value) {
  if (typeof value === 'number' && !Number.isSafeInteger(value)) throw Error('Use BigInt for 64-bit values');
  const result = BigInt(value);
  if (result < 0n || result > 0xffffffffffffffffn) throw Error('Expected a uint64');
  return result;
}
function words(...values) {
  const result = Buffer.alloc(values.length * 4);
  values.forEach((value, index) => result.writeUInt32LE(u32(value), index * 4));
  return result;
}
function qwords(...values) {
  const result = Buffer.alloc(values.length * 8);
  values.forEach((value, index) => result.writeBigUInt64LE(u64(value), index * 8));
  return result;
}
function id(resource) { return u32(typeof resource === 'number' ? resource : resource.id); }
function hex(bytes) {
  if (typeof bytes !== 'string' || !/^(?:[0-9a-f]{2})*$/i.test(bytes)) throw Error('Expected even-length hexadecimal bytes');
  return Buffer.from(bytes, 'hex');
}
function attr(value) {
  if (typeof value === 'number') return u32(value);
  if (typeof value !== 'string') throw Error('Expected buffer attributes');
  return value.split('|').reduce((result, name) => {
    if (!Object.hasOwn(bufferAttributes, name)) throw Error(`Unknown buffer attribute ${name}`);
    return result | bufferAttributes[name];
  }, 0);
}
function descriptor(bytes, at = 0) {
  if (at + 32 > bytes.length) throw Error('Truncated resource descriptor');
  const kind = bytes.readUInt32LE(at + 4);
  if (kind < 1 || kind > 4) throw Error('Unknown resource kind');
  return {
    id: bytes.readUInt32LE(at), kind: ['unused', 'service', 'handle', 'buffer', 'jit'][kind],
    handle: bytes.readUInt32LE(at + 8), objectId: bytes.readUInt32LE(at + 12),
    address: bytes.readBigUInt64LE(at + 16), size: bytes.readBigUInt64LE(at + 24),
  };
}
function oneDescriptor(bytes) {
  if (bytes.length !== 32) throw Error('Unexpected resource descriptor length');
  return descriptor(bytes);
}
function oneU64(bytes) {
  if (bytes.length !== 8) throw Error('Unexpected uint64 reply length');
  return bytes.readBigUInt64LE();
}

function encodeIpc(resource, options) {
  const input = options.data === undefined ? Buffer.alloc(0) : Buffer.from(options.data);
  const outputSize = u32(options.outSize ?? 0);
  const buffers = options.buffers ?? [];
  const handles = options.handles ?? [];
  const outHandles = options.outHandles ?? [];
  const objects = options.objects ?? [];
  const outObjects = u32(options.outObjects ?? 0);
  if (input.length > 128 || outputSize > 128 ||
      [buffers.length, handles.length, outHandles.length, objects.length, outObjects].some(n => n > 4)) {
    throw Error('IPC supports 128 inline bytes and up to four of each descriptor kind');
  }
  if (resource?.objectId && ((objects.length && input.length % 4) || (outObjects && outputSize % 4))) {
    throw Error('Domain-object arrays require four-byte-aligned inline data');
  }
  const bufferInfo = buffers.map(buffer => {
    const flags = attr(buffer.attr);
    const data = buffer.data === undefined ? Buffer.alloc(0) : Buffer.from(buffer.data);
    const size = u32(buffer.size ?? data.length);
    const transport = flags & (4 | 8 | 32);
    if (!size || size > MAX_BUFFER || data.length > size || !(flags & 3) || (flags & ~255) ||
        ![4, 8, 32].includes(transport) ||
        ((flags & 16) && (transport !== 8 || !(flags & 2))) ||
        ((flags & (64 | 128)) && transport === 8) ||
        (flags & (64 | 128)) === (64 | 128) || (transport === 8 && size > 65535)) {
      throw Error('Invalid IPC buffer attributes or length');
    }
    const padded = Buffer.alloc(size);
    if (flags & 1) data.copy(padded);
    return {flags, size, data: padded};
  });
  const handleAttrs = outHandles.map(value => {
    const result = typeof value === 'string' ? {copy: 1, move: 2}[value] : value;
    if (result !== 1 && result !== 2) throw Error('Expected copy or move output-handle attribute');
    return result;
  });
  const payload = Buffer.concat([
    words(id(resource), u32(options.commandId), u32(options.context ?? 0), options.sendPid ? 1 : 0,
      input.length, outputSize, buffers.length, handles.length, outHandles.length,
      objects.length, outObjects, 0),
    ...bufferInfo.map(buffer => words(buffer.flags, buffer.size)),
    words(...handles.map(id)), words(...handleAttrs), words(...objects.map(id)),
    input, ...bufferInfo.map(buffer => buffer.data),
  ]);
  if (payload.length > MAX_PACKET) throw Error('IPC packet exceeds one MiB');
  return payload;
}

function decodeIpc(bytes) {
  if (bytes.length < 20) throw Error('Truncated IPC reply');
  const result = bytes.readUInt32LE();
  const inlineSize = bytes.readUInt32LE(4);
  const objectCount = bytes.readUInt32LE(8);
  const handleCount = bytes.readUInt32LE(12);
  const bufferCount = bytes.readUInt32LE(16);
  if (inlineSize > 128 || [objectCount, handleCount, bufferCount].some(n => n > 4)) throw Error('Invalid IPC reply counts');
  const metadataSize = 20 + 32 * (objectCount + handleCount) + 4 * bufferCount;
  if (metadataSize + inlineSize > bytes.length) throw Error('Truncated IPC reply metadata');
  let at = 20;
  const objects = [], handles = [], sizes = [];
  for (let i = 0; i < objectCount; i++, at += 32) objects.push(descriptor(bytes, at));
  for (let i = 0; i < handleCount; i++, at += 32) handles.push(descriptor(bytes, at));
  for (let i = 0; i < bufferCount; i++, at += 4) {
    const size = bytes.readUInt32LE(at);
    if (!size || size > MAX_BUFFER) throw Error('Invalid IPC reply buffer size');
    sizes.push(size);
  }
  const data = Buffer.from(bytes.subarray(at, at + inlineSize));
  at += inlineSize;
  const buffers = sizes.map(size => {
    if (at + size > bytes.length) throw Error('Truncated IPC output buffer');
    const buffer = Buffer.from(bytes.subarray(at, at + size));
    at += size;
    return buffer;
  });
  if (at !== bytes.length) throw Error('Trailing bytes in IPC reply');
  return {result, data, objects, handles, buffers};
}

async function connect({host, port = 8090, timeout = 15000}) {
  if (typeof host !== 'string' || !host || /[\s/\\]/.test(host)) throw Error('Provide a console hostname or IP');
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw Error('Invalid port');
  if (!Number.isSafeInteger(timeout) || timeout < 1) throw Error('Invalid timeout');
  const socket = net.createConnection({host, port});
  socket.setNoDelay(true);
  let pending = null, incoming = Buffer.alloc(0), closed = false, sequence = 0;
  let queue = Promise.resolve();
  function fail(error) {
    closed = true;
    if (pending) {
      clearTimeout(pending.timer);
      pending.reject(error);
      pending = null;
    }
    socket.destroy();
  }
  socket.on('error', error => fail(error));
  socket.on('close', () => fail(Error('Bridge disconnected. The last command outcome may be unknown. Reconnect creates a new resource session.')));
  socket.on('data', chunk => {
    if (!pending) return fail(Error('Unexpected reply while no command was pending'));
    if (incoming.length + chunk.length > MAX_PACKET + 24) return fail(Error('Oversized bridge reply'));
    incoming = Buffer.concat([incoming, chunk]);
    if (incoming.length < 24) return;
    const length = incoming.readUInt32LE(12);
    if (incoming.readUInt32LE() !== MAGIC || incoming.readUInt16LE(4) !== VERSION ||
        incoming.readUInt16LE(6) !== pending.operation || incoming.readUInt32LE(8) !== pending.sequence ||
        incoming.readUInt32LE(20) || length > MAX_PACKET) return fail(Error('Invalid bridge reply header'));
    if (incoming.length < 24 + length) return;
    if (incoming.length !== 24 + length) return fail(Error('Unexpected extra bridge reply data'));
    const result = incoming.readUInt32LE(16);
    const payload = Buffer.from(incoming.subarray(24));
    const done = pending;
    pending = null; incoming = Buffer.alloc(0); clearTimeout(done.timer);
    if (result) {
      const error = Error(`Bridge result 0x${result.toString(16)}${errors[result] ? ': ' + errors[result] : ''}`);
      error.result = result;
      done.reject(error);
    } else done.resolve(payload);
  });
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => { socket.destroy(); reject(Error('Bridge connection timed out')); }, timeout);
    socket.once('connect', () => { clearTimeout(timer); resolve(); });
    socket.once('error', error => { clearTimeout(timer); reject(error); });
  });
  function request(operation, payload = Buffer.alloc(0)) {
    const copy = Buffer.from(payload);
    if (copy.length > MAX_PACKET) return Promise.reject(Error('Request exceeds one MiB'));
    const result = queue.then(() => new Promise((resolve, reject) => {
      if (closed || sequence === 0xffffffff) return reject(Error('Bridge connection is closed'));
      const header = Buffer.alloc(24);
      header.writeUInt32LE(MAGIC); header.writeUInt16LE(VERSION, 4);
      header.writeUInt16LE(operation, 6); header.writeUInt32LE(++sequence, 8);
      header.writeUInt32LE(copy.length, 12);
      const timer = setTimeout(() => fail(Error('Command timed out. Outcome unknown. No command was retried.')), timeout);
      pending = {operation, sequence, resolve, reject, timer};
      socket.write(Buffer.concat([header, copy]));
    }));
    queue = result.catch(() => {});
    return result;
  }
  const bridge = {
    status: async () => JSON.parse((await request(operations.status)).toString('utf8')),
    symbols: async () => Object.fromEntries(Object.entries(JSON.parse((await request(operations.symbols)).toString('utf8'))).map(([name, address]) => [name, BigInt(address)])),
    syscallHints: async () => {
      const bytes = await request(operations.hints);
      if (bytes.length !== 256) throw Error('Unexpected syscall-hint reply size');
      return [...bytes].flatMap((available, index) => available ? [index] : []);
    },
    resources: async () => {
      const bytes = await request(operations.resources);
      if (bytes.length % 32) throw Error('Invalid resource-list length');
      return Array.from({length: bytes.length / 32}, (_, index) => descriptor(bytes, index * 32));
    },
    getInfo: async (type, subtype = 0n, resource = 0) => oneU64(await request(operations.getInfo, Buffer.concat([words(u32(type), id(resource)), qwords(subtype)]))),
    queryMemory: async address => {
      const bytes = await request(operations.query, qwords(address));
      if (bytes.length !== 44) throw Error('Unexpected memory-query reply size');
      return {address: bytes.readBigUInt64LE(), size: bytes.readBigUInt64LE(8),
        type: bytes.readUInt32LE(16), attributes: bytes.readUInt32LE(20),
        permissions: bytes.readUInt32LE(24), ipcReferences: bytes.readUInt32LE(28),
        deviceReferences: bytes.readUInt32LE(32), pageInfo: bytes.readUInt32LE(40)};
    },
    readMemory: async (address, size) => {
      if (!Number.isInteger(size) || size < 1 || size > MAX_BUFFER) throw Error('Read size must be 1..262144');
      const bytes = await request(operations.read, Buffer.concat([qwords(address), words(size)]));
      if (bytes.length !== size) throw Error('Short bridge memory read');
      return bytes;
    },
    allocate: async size => oneDescriptor(await request(operations.alloc, words(size))),
    writeAllocation: async (resource, offset, data) => {
      const copy = Buffer.from(data);
      const bytes = await request(operations.write, Buffer.concat([words(id(resource), offset), copy]));
      if (bytes.length !== 4 || bytes.readUInt32LE() !== copy.length) throw Error('Unexpected allocation-write reply');
      return copy.length;
    },
    openService: async name => oneDescriptor(await request(operations.open, Buffer.from(name, 'ascii'))),
    henResource: async name => oneDescriptor(await request(operations.hen, Buffer.from(name, 'ascii'))),
    clone: async resource => oneDescriptor(await request(operations.clone, words(id(resource)))),
    domain: async resource => oneDescriptor(await request(operations.domain, words(id(resource)))),
    asService: async resource => oneDescriptor(await request(operations.asService, words(id(resource)))),
    closeResource: async resource => {
      const bytes = await request(operations.close, words(id(resource)));
      if (bytes.length) throw Error('Unexpected close reply');
    },
    ipc: async (resource, options) => decodeIpc(await request(operations.ipc, encodeIpc(resource, options))),
    jit: async code => {
      const bytes = Buffer.from(code);
      if (!bytes.length || bytes.length > MAX_BUFFER || bytes.length % 4) throw Error('AArch64 code must be a nonempty multiple of four bytes, at most 256 KiB');
      return oneDescriptor(await request(operations.jit, bytes));
    },
    call: async (address, ...args) => {
      if (args.length > 8) throw Error('Native calls accept at most eight integer/pointer arguments');
      const padded = [...args];
      while (padded.length < 8) padded.push(0n);
      return oneU64(await request(operations.call, qwords(address, ...padded)));
    },
    close: async () => {
      await queue;
      if (closed) return;
      await new Promise(resolve => {
        const timer = setTimeout(() => { socket.destroy(); resolve(); }, 3000);
        socket.once('close', () => { clearTimeout(timer); resolve(); });
        socket.end();
      });
    },
    abort: () => socket.destroy(),
  };
  return bridge;
}

module.exports = {connect, words, qwords, hex, attr, u32, u64, encodeIpc, decodeIpc, operations, MAGIC, VERSION, MAX_PACKET};
