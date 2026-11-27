// tools/emulator-shot.mjs —— 用 CDP 驱动本机 Chrome，把固件塞进 FoloToy 模拟器并截图。
//
// 用法: node tools/emulator-shot.mjs <firmware.bin> <out.png> [按键序列] [等待毫秒]
//   按键序列用逗号分隔，支持: ok / up / down / ok-long
//   例: node tools/emulator-shot.mjs build/FocusTimer-full.bin shot.png "ok,up" 6000
//
// 依赖: 本机 Chrome + 运行中的模拟器（默认 http://127.0.0.1:4190）
import { spawn } from "node:child_process";
import { writeFileSync } from "node:fs";
import { setTimeout as sleep } from "node:timers/promises";

const CHROME_CANDIDATES = [
  "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
  `${process.env.LOCALAPPDATA}\\Google\\Chrome\\Application\\chrome.exe`,
  "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
  "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
];

const firmware = process.argv[2];
const out = process.argv[3] ?? "emulator-shot.png";
const keys = (process.argv[4] ?? "").split(",").map((s) => s.trim()).filter(Boolean);
const bootWait = Number(process.argv[5] ?? 9000);
const EMULATOR = process.env.EMULATOR_URL ?? "http://127.0.0.1:4190/";
const PORT = Number(process.env.CDP_PORT ?? 9411);

if (!firmware) {
  console.error("用法: node emulator-shot.mjs <firmware.bin> <out.png> [ok,up,down] [bootWaitMs]");
  process.exit(2);
}

const { existsSync } = await import("node:fs");
const chromePath = CHROME_CANDIDATES.find((p) => existsSync(p));
if (!chromePath) {
  console.error("没找到 Chrome/Edge");
  process.exit(2);
}

const chrome = spawn(chromePath, [
  "--headless=new",
  `--remote-debugging-port=${PORT}`,
  `--user-data-dir=${process.env.TEMP ?? "."}\\passport-emulator-shot`,
  "--window-size=1280,1024",
  "--no-first-run",
  "--no-default-browser-check",
  "--disable-gpu",
  "--disable-extensions",
  "--mute-audio",
  EMULATOR,
], { stdio: "ignore" });

const origin = new URL(EMULATOR).origin;

async function findPage() {
  for (let i = 0; i < 60; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
      const p = list.find((t) => t.type === "page" && t.url.startsWith(origin));
      if (p?.webSocketDebuggerUrl) return p;
    } catch {}
    await sleep(400);
  }
  throw new Error("Chrome devtools 不可达");
}

const page = await findPage();
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });

let seq = 0;
const pending = new Map();
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) {
    const { resolve, reject } = pending.get(m.id);
    pending.delete(m.id);
    m.error ? reject(new Error(JSON.stringify(m.error))) : resolve(m.result);
  }
};
const send = (method, params = {}) =>
  new Promise((resolve, reject) => {
    const id = ++seq;
    pending.set(id, { resolve, reject });
    ws.send(JSON.stringify({ id, method, params }));
  });
const evaluate = async (expression) =>
  (await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true })).result?.value;

const KEYS = {
  ok: ["Enter", 13, "Enter"],
  up: ["ArrowUp", 38, "ArrowUp"],
  down: ["ArrowDown", 40, "ArrowDown"],
};
async function press(name) {
  const long = name === "ok-long";
  const [code, vk, key] = KEYS[long ? "ok" : name] ?? KEYS.ok;
  await send("Input.dispatchKeyEvent", { type: "rawKeyDown", windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, code, key, autoRepeat: long });
  if (long) await sleep(1200);
  await send("Input.dispatchKeyEvent", { type: "keyUp", windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, code, key });
  await sleep(350);
}

await send("Page.enable");
await send("Runtime.enable");
await send("DOM.enable");

let ready = false;
for (let i = 0; i < 80; i++) {
  ready = await evaluate(
    `(() => { const b = document.querySelector('#firmware-upload'); return !!b && !b.hasAttribute('disabled'); })()`,
  );
  if (ready) break;
  await sleep(400);
}
if (!ready) throw new Error("模拟器运行时未就绪（按钮一直是 disabled）");

const doc = await send("DOM.getDocument", { depth: -1 });
const input = await send("DOM.querySelector", { nodeId: doc.root.nodeId, selector: "#firmware-file" });
if (!input.nodeId) throw new Error("页面上找不到 #firmware-file（模拟器版本变了？）");
await send("DOM.setFileInputFiles", { nodeId: input.nodeId, files: [firmware] });

await sleep(bootWait);

// 关掉首次进入的“模拟器说明”弹窗
await evaluate(`(() => {
  const b = [...document.querySelectorAll('button')].find((x) => (x.textContent || '').includes('知道了'));
  if (b) b.click();
  return !!b;
})()`);
await sleep(600);

for (const k of keys) await press(k);

const shot = await send("Page.captureScreenshot", { format: "png" });
writeFileSync(out, Buffer.from(shot.data, "base64"));
console.log(`screenshot: ${out}`);

ws.close();
chrome.kill();
process.exit(0);
