// Direct WebSocket probe. Read the credential from a file, never argv or logs.
import { readFileSync } from "node:fs";
import { performance } from "node:perf_hooks";
const [url, file] = process.argv.slice(2);
const count = Number(process.argv[4] || 50);
if (!Number.isInteger(count) || count < 1 || count > 1000) throw new Error("invalid probe count");
if (!url || !file || !url.startsWith("wss://")) throw new Error("usage: edge-probe.mjs wss://host/probe token-file");
const token = readFileSync(file, "utf8").trim();
const started = performance.now();
const ws = new WebSocket(url, ["ccproxy-v1", "ccproxy-auth." + token]);
ws.binaryType = "arraybuffer";
const samples = [];
let expected = 0, sent = 0, timer;
const arm = () => { clearTimeout(timer); timer = setTimeout(() => { console.error(`WebSocket probe timeout after ${samples.length} samples`); ws.close(); process.exit(1); }, 15000); };
const send = () => { arm(); sent = performance.now(); ws.send(new Uint8Array([expected])); };
arm();
ws.addEventListener("open", () => { console.log(`connected_ms=${(performance.now() - started).toFixed(2)}`); send(); });
ws.addEventListener("error", () => { clearTimeout(timer); console.error("WebSocket connection failed"); process.exit(1); });
ws.addEventListener("message", event => {
  const value = new Uint8Array(event.data);
  if (value.length !== 1 || value[0] !== expected) { console.error("Unexpected probe response"); process.exit(1); }
  if (expected) samples.push(performance.now() - sent);
  if (++expected <= count) send();
  else {
    clearTimeout(timer); samples.sort((a,b) => a-b);
    console.log(JSON.stringify({ samples: count, p50_ms: +(samples[Math.floor((count-1)/2)]/2 + samples[Math.floor(count/2)]/2).toFixed(3),
      p95_ms: +samples[Math.ceil(count*.95)-1].toFixed(3), max_ms: +samples.at(-1).toFixed(3) }));
    ws.close();
  }
});
ws.addEventListener("close", event => { clearTimeout(timer); if (samples.length !== count) { console.error(`Probe closed early code=${event.code} samples=${samples.length}`); process.exit(1); } });
