// The service's upload path (service/src/runs.ts) on a .crd, minus signing and the database.
import { plugin } from "bun";
import fs from "node:fs";
plugin({
  name: "wasm-module",
  setup(build) {
    build.onLoad({ filter: /\.wasm$/ }, (args) => ({
      exports: { default: new WebAssembly.Module(fs.readFileSync(args.path)) },
      loader: "object",
    }));
  },
});
const S = process.argv[2]; // service dir
const { decodeReplay, inflateReplay, REPLAY_RULES } = await import(`${S}/src/replay.ts`);
const { encodeTransport } = await import(`${S}/src/transport.ts`);
const { verifyRun } = await import(`${S}/src/verify.ts`);
const { outcomeReasons, unrankedReasons, rankedScore } = await import(`${S}/src/ranked.ts`);
const file = new Uint8Array(fs.readFileSync(process.argv[3]));
const replay = decodeReplay(inflateReplay(file));
console.log("format", replay.format_version, "rules", replay.rules, "(service", REPLAY_RULES + ")", "pilot", JSON.stringify(replay.pilot));
const reasons = [...unrankedReasons(replay.run), ...outcomeReasons(replay.run, replay.result)];
console.log("unranked reasons:", JSON.stringify(reasons));
const transport = encodeTransport(replay);
if (process.argv[4]) console.log("transport equals ours:", Buffer.from(transport).equals(fs.readFileSync(process.argv[4])));
const v = await verifyRun({}, replay, transport);
console.log("verdict:", v.ok ? "ok" : v.reason, "score", rankedScore(replay.run, replay.result));
