#!/usr/bin/env node
'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const repl = require('node:repl');
const helpers = require('./client.cjs');

function print(value) {
  console.log(JSON.stringify(value, (_, item) => {
    if (typeof item === 'bigint') return '0x' + item.toString(16);
    if (item?.type === 'Buffer' && Array.isArray(item.data)) return Buffer.from(item.data).toString('hex');
    return item;
  }, 2));
}

function help() {
  console.log(`HEN command bridge
Usage: node cli.cjs --host ADDRESS COMMAND [arguments]
The NRO must be open. Connects directly over your LAN.

  status                         Show bridge and HEN status
  hints                          Show available syscall hints
  symbols                        Show callable native wrapper addresses
  query ADDRESS                  Query a region in the bridge process
  maps [START] [COUNT]            Walk at most COUNT regions (default 256)
  read ADDRESS LENGTH NEW_FILE   Read bridge-process memory to a new PC file
  get-info TYPE [SUBTYPE]        Call svcGetInfo for the bridge process
  ipc RECIPE.json                Issue one explicitly described CMIF request
  script SCRIPT.cjs [args...]    Run a PC script with a persistent bridge session
  repl                           Interactive JavaScript with bridge and h

Options: --port PORT, --timeout MILLISECONDS
IPC and native code can modify persistent state through invoked services.
Commands run once. After a timeout, inspect the outcome before issuing another.
`);
}

async function main(args) {
  const options = {host: process.env.HCB_HOST};
  while (args[0]?.startsWith('--')) {
    const option = args.shift();
    if (option === '--help') { help(); return; }
    if (!['--host', '--port', '--timeout'].includes(option) || !args.length) throw Error('Unknown or incomplete option ' + option);
    const value = args.shift();
    if (option === '--host') options.host = value;
    else options[option.slice(2)] = Number(value);
  }
  const command = args.shift();
  if (!command || command === 'help') { help(); return; }
  if (!options.host) throw Error('Use --host ADDRESS or HCB_HOST');
  let recipe, script;
  if (command === 'ipc') {
    if (args.length !== 1) throw Error('ipc requires one recipe JSON file');
    recipe = JSON.parse(await fs.readFile(args[0], 'utf8'));
    if (Boolean(recipe.service) === Boolean(recipe.hen)) throw Error('Recipe must name exactly one service or hen resource');
  }
  if (command === 'script') {
    if (!args.length) throw Error('script requires a CommonJS module');
    script = require(path.resolve(args.shift()));
    if (typeof script !== 'function') throw Error('Script must export an async function');
  }
  const allowed = ['status', 'hints', 'symbols', 'query', 'maps', 'read', 'get-info', 'ipc', 'script', 'repl'];
  if (!allowed.includes(command)) throw Error('Unknown command ' + command);
  const bridge = await helpers.connect(options);
  const interrupt = () => { bridge.abort(); process.exitCode = 130; };
  process.on('SIGINT', interrupt);
  try {
    if (command === 'status') print(await bridge.status());
    else if (command === 'hints') print((await bridge.syscallHints()).map(value => '0x' + value.toString(16)));
    else if (command === 'symbols') print(await bridge.symbols());
    else if (command === 'query') {
      if (args.length !== 1) throw Error('query requires an address');
      print(await bridge.queryMemory(BigInt(args[0])));
    } else if (command === 'maps') {
      if (args.length > 2) throw Error('maps accepts a starting address and region limit');
      let address = helpers.u64(args[0] ?? '0');
      const count = Number(args[1] ?? 256);
      if (!Number.isInteger(count) || count < 1 || count > 4096) throw Error('Region count must be 1..4096');
      for (let i = 0; i < count; i++) {
        const region = await bridge.queryMemory(address);
        print(region);
        const next = region.address + region.size;
        if (!region.size || next <= address || next > 0xffffffffffffffffn) break;
        address = next;
      }
    } else if (command === 'get-info') {
      if (args.length < 1 || args.length > 2) throw Error('get-info requires a type and optional subtype');
      print(await bridge.getInfo(Number(args[0]), BigInt(args[1] ?? '0')));
    } else if (command === 'read') {
      if (args.length !== 3) throw Error('read requires address, length, and a new output filename');
      const bytes = await bridge.readMemory(BigInt(args[0]), Number(args[1]));
      await fs.writeFile(args[2], bytes, {flag: 'wx'});
      print({file: args[2], bytes: bytes.length});
    } else if (command === 'ipc') {
      let resource = recipe.hen ? await bridge.henResource(recipe.hen) : await bridge.openService(recipe.service);
      if (recipe.domain) resource = await bridge.domain(resource);
      const options = {
        commandId: recipe.commandId, context: recipe.context, sendPid: recipe.sendPid,
        data: helpers.hex(recipe.inHex ?? ''), outSize: recipe.outSize,
        outObjects: recipe.outObjects, outHandles: recipe.outHandles,
        buffers: (recipe.buffers ?? []).map(buffer => ({attr: buffer.attr, size: buffer.size, data: helpers.hex(buffer.inHex ?? '')})),
      };
      print(await bridge.ipc(resource, options));
    } else if (command === 'script') {
      await script({bridge, h: helpers, args, print});
    } else if (command === 'repl') {
      console.log('Use await bridge.status(), await bridge.openService(name), and bridge.ipc(). Type .exit to disconnect.');
      await new Promise(resolve => {
        const session = repl.start({prompt: 'hen> '});
        session.context.bridge = bridge;
        session.context.h = helpers;
        session.on('exit', resolve);
      });
    }
  } finally {
    process.removeListener('SIGINT', interrupt);
    await bridge.close();
  }
}

if (require.main === module) main(process.argv.slice(2)).catch(error => { console.error(error.message); process.exitCode = 1; });
module.exports = {main, print};
