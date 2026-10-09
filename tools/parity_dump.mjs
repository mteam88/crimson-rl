import fs from "node:fs";
import { loadCore, init, decode, step, state, index } from "../upstream/crimson/crimson-core/checks/engine.mjs";
const [wasm, bin, fieldsJson, out] = process.argv.slice(2);
const fields = JSON.parse(fieldsJson).map((n) => index.get(n));
const e = loadCore(wasm); const run = decode(fs.readFileSync(bin)); init(e, run.config);
const rows = Buffer.alloc((run.records.length + 1) * fields.length * 4); let n = 0;
const put = () => { const s = state(e); for (const f of fields) { rows.writeUInt32LE(s.readUInt32LE(f * 4), n * 4); n++; } };
put();
for (const r of run.records) { if (!step(e, r)) break; put(); }
fs.writeFileSync(out, rows.subarray(0, n * 4)); console.log("rows", n / fields.length);
