'use strict';
const assert = require('node:assert/strict');

module.exports = async ({bridge, h, args, print}) => {
  const status = await bridge.status();
  print(status);
  const hen = await bridge.henResource('hen');
  try {
    const reply = await bridge.ipc(hen, {commandId: 0, outSize: 16});
    assert.equal(reply.result, 0, 'HEN GetStatus failed');
    assert.equal(reply.data.readUInt32LE(8), status.henProtocol);
    print({henStatusViaIpc: 'passed', protocol: reply.data.readUInt32LE(8)});
  } finally { await bridge.closeResource(hen); }
  const allocation = await bridge.allocate(64);
  try {
    const bytes = h.hex('484342312d736d6f6b65');
    await bridge.writeAllocation(allocation, 8, bytes);
    assert.deepEqual(await bridge.readMemory(allocation.address + 8n, bytes.length), bytes);
    print({allocationReadback: 'passed', region: await bridge.queryMemory(allocation.address)});
  } finally { await bridge.closeResource(allocation); }
  if (args.includes('--native')) {
    // add x0, x0, x1; ret. Exercises only the bridge process's JIT mapping.
    const code = await bridge.jit(h.hex('0000018bc0035fd6'));
    try {
      const result = await bridge.call(code.address, 19n, 23n);
      assert.equal(result, 42n);
      print({nativeAddition: 'passed', result});
    } finally { await bridge.closeResource(code); }
  }
  assert.deepEqual(await bridge.resources(), []);
  print({smoke: 'passed'});
};
