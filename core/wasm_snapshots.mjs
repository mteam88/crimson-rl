// Runs a transport (config + tick records, upstream's native-core stdin format) through the WASM core,
// the module crimson.land's verifier runs, and writes snapshots like the native core: u32 count, then fields.
// Usage: node core/wasm_snapshots.mjs <core.wasm> < transport > snapshots
import fs from "node:fs";
import { decode, init, loadCore, step } from "../upstream/crimson/crimson-core/checks/engine.mjs";

const e = loadCore(process.argv[2]);
const input = fs.readFileSync(0);
const { config, records } = decode(input);
init(e, config);
// Written as they come: a long run's snapshots run to gigabytes.
const emit = () => {
  const n = e.portable_snapshot();
  const head = Buffer.alloc(4);
  head.writeUInt32LE(n);
  fs.writeSync(1, head);
  fs.writeSync(1, new Uint8Array(e.memory.buffer, e.portable_output(), n * 4));
};
emit();
for (const r of records) {
  if (!step(e, r)) break;
  emit();
}
